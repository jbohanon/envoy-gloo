#include "source/extensions/filters/http/aws_lambda/sts_fetcher.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/regex.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AwsLambda {

namespace {

class StsFetcherImpl : public StsFetcher,
                       public Logger::Loggable<Logger::Id::aws>,
                       public Http::AsyncClient::Callbacks {
public:
  StsFetcherImpl(Upstream::ClusterManager &cm, Api::Api &api)
      : cm_(cm), api_(api){
    ENVOY_LOG(trace, "{}", __func__);
  }

  ~StsFetcherImpl() override { cancel(); }

  void cancel() override {
    if (request_ && !complete_) {
      request_->cancel();
      ENVOY_LOG(debug, "assume role with token [uri = {}]: canceled",
                uri_->uri());
    }
    reset();
  }

  void fetch(const envoy::config::core::v3::HttpUri &uri,
             const absl::string_view role_arn,
             const absl::string_view web_token,
             StsCredentialsConstSharedPtr creds,
             StsFetcher::Callbacks *callbacks) override {
    ENVOY_LOG(trace, "{}", __func__);
    ASSERT(callbacks_ == nullptr);

    complete_ = false;
    callbacks_ = callbacks;
    uri_ = &uri;

    set_role(role_arn);
  
    // Check if cluster is configured, fail the request if not.
    // Otherwise cm_.httpAsyncClientForCluster will throw exception.
    const auto thread_local_cluster = cm_.getThreadLocalCluster(uri.cluster());
    
    if (thread_local_cluster == nullptr) {
      ENVOY_LOG(error,
                "{}: assume role with token [uri = {}] failed: [cluster = {}] "
                "is not configured",
                __func__, uri.uri(), uri.cluster());
      complete_ = true;
      callbacks_->onFailure(CredentialsFailureStatus::ClusterNotFound);
      reset();
      return;
    }

    Http::RequestMessagePtr message = Http::Utility::prepareHeaders(uri);
    message->headers().setReferenceMethod(
        Http::Headers::get().MethodValues.Post);
    message->headers().setContentType(
        Http::Headers::get().ContentTypeValues.FormUrlEncoded);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         api_.timeSource().systemTime().time_since_epoch())
                         .count();
    auto options = Http::AsyncClient::RequestOptions().setTimeout(
        std::chrono::milliseconds(
            DurationUtil::durationToMilliseconds(uri.timeout())));

    // Short form for web_token assume role
    if (creds == nullptr){
       const std::string body =
        fmt::format(StsFormatString, role_arn, now, web_token);
      message->body().add(body);
      ENVOY_LOG(debug, "assume role with token from [uri = {}]: start",
                                                                uri_->uri());
      request_ = thread_local_cluster->httpAsyncClient().send(
                                           std::move(message),*this, options);
      return;
    }
    
    // Chained assumption specifics 
    const std::string body = fmt::format(StsChainedFormatString, role_arn, now);
    message->body().add(body);
    // this call resets the sha so that our authenticator is in a fresh state
    // to be reused.
    AwsAuthenticator aws_authenticator(api_.timeSource(),
                                             &AWSStsHeaderNames::get().Service);
    aws_authenticator.init(&creds->accessKeyId().value(), 
        &creds->secretAccessKey().value(), &creds->sessionToken().value());
    aws_authenticator.updatePayloadHash(message->body());
    auto& hdrs = message->headers();
    // TODO(nfuden) allow for Region this to be overridable. 
    // DefaultRegion is gauranteed to be available but an override may be faster
    aws_authenticator.sign(&hdrs, HeadersToSign, DefaultRegion);
    // Log the accessKey but not the secret. This is to show that we have valid 
    // credentials but does not leak anything secret. This is due to our
    // sessions being 
    ENVOY_LOG(trace, "assume chained [accesskey={}] ",
                creds->accessKeyId().value());
    ENVOY_LOG(debug, "assume chained role from [uri = {}]: start", uri_->uri());
    request_ = thread_local_cluster->httpAsyncClient().send(
                                            std::move(message), *this, options);
  }

  // HTTP async receive methods
  void onSuccess(const Http::AsyncClient::Request &,
                 Http::ResponseMessagePtr &&response) override {
    complete_ = true;
    const uint64_t status_code =
        Http::Utility::getResponseStatus(response->headers());
    if (status_code == enumToInt(Http::Code::OK)) {
      ENVOY_LOG(debug, "{}: assume role with token [uri = {}]: success",
                __func__, uri_->uri());
      if (response->body().length() > 0) {
        const auto len = response->body().length();
        const auto body = absl::string_view(
            static_cast<char *>(response->body().linearize(len)), len);
        callbacks_->onSuccess(body);

      } else {
        ENVOY_LOG(debug, "{}: assume role with token [uri = {}]: body is empty",
                  __func__, uri_->uri());
        callbacks_->onFailure(CredentialsFailureStatus::Network);
      }
    } else {
      if ((status_code >= 400) && (status_code <= 403) && (response->body().length() > 0)) {
        const auto len = response->body().length();
        const auto body = absl::string_view(
            static_cast<char *>(response->body().linearize(len)), len);
        ENVOY_LOG(debug, "{}: StatusCode: {}, Body: \n {}", __func__,
                  status_code, body);
        // TODO: cover more AWS error cases
        if (body.find(ExpiredTokenError) != std::string::npos) {
          callbacks_->onFailure(CredentialsFailureStatus::ExpiredToken);
        } else {
          callbacks_->onFailure(CredentialsFailureStatus::Network);
        }
        // TODO: parse the error string. Example:
        /*
          <ErrorResponse
          xmlns="http://webservices.amazon.com/AWSFault/2005-15-09"> <Error>
              <Type>Sender</Type>
              <Code>InvalidAction</Code>
              <Message>Could not find operation AssumeRoleWithWebIdentity for
          version NO_VERSION_SPECIFIED</Message>
            </Error>
            <RequestId>72168399-bcdd-4248-bf57-bf5d4a6dc07d</RequestId>
          </ErrorResponse>
        */
      } else {
        ENVOY_LOG(
            debug,
            "{}: assume role with token [uri = {}]: response status code {}",
            __func__, uri_->uri(), status_code);
        ENVOY_LOG(trace, "{}: headers: {}", __func__, response->headers());
        callbacks_->onFailure(CredentialsFailureStatus::Network);
      }
    }
    reset();
  }

  void onFailure(const Http::AsyncClient::Request &,
                 Http::AsyncClient::FailureReason reason) override {
    ENVOY_LOG(debug, "{}: assume role with token [uri = {}]: network error {}",
              __func__, uri_->uri(), enumToInt(reason));
    complete_ = true;
    callbacks_->onFailure(CredentialsFailureStatus::Network);
    reset();
  }

  void onBeforeFinalizeUpstreamSpan(Tracing::Span &,
                                    const Http::ResponseHeaderMap *) override {}

private:
  Upstream::ClusterManager &cm_;
  Api::Api &api_;
  bool complete_{};
  StsFetcher::Callbacks *callbacks_{};
  const envoy::config::core::v3::HttpUri *uri_{};
  Http::AsyncClient::Request *request_{};
  absl::string_view role_arn_;

  class AWSStsHeaderValues {
  public:
    const std::string Service{"sts"};
    const Http::LowerCaseString DateHeader{"x-amz-date"};
    const Http::LowerCaseString FunctionError{"x-amz-function-error"};
  };
  typedef ConstSingleton<AWSStsHeaderValues> AWSStsHeaderNames;
  const HeaderList HeadersToSign =
    AwsAuthenticator::createHeaderToSign(
        { 
        Http::Headers::get().ContentType,
        AWSStsHeaderNames::get().DateHeader,
        Http::Headers::get().HostLegacy,
        }
      );

  // work around having consts being passed around.
  void set_role(const absl::string_view role_arn){
    role_arn_ = role_arn;
  }

  const std::string DefaultRegion = "us-east-1";

  void reset() {
    request_ = nullptr;
    callbacks_ = nullptr;
    uri_ = nullptr;
  }
};
} // namespace

StsFetcherPtr StsFetcher::create(Upstream::ClusterManager &cm, Api::Api &api) {
  return std::make_unique<StsFetcherImpl>(cm, api);
}

} // namespace AwsLambda
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
