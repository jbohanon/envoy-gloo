#pragma once

#include <map>

#include "envoy/buffer/buffer.h"
#include "envoy/http/header_map.h"

#include "source/common/common/base64.h"

#include "source/extensions/filters/http/transformation/transformer.h"

// clang-format off
#include "nlohmann/json.hpp"
#include "inja/inja.hpp"
// clang-format on

#include "api/envoy/config/filter/http/transformation/v2/transformation_filter.pb.validate.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Transformation {

using GetBodyFunc = std::function<const std::string &()>;

class TransformerInstance {
public:
  TransformerInstance(
      const Http::RequestOrResponseHeaderMap &header_map,
      const Http::RequestHeaderMap *request_headers, GetBodyFunc &body,
      const std::unordered_map<std::string, absl::string_view> &extractions,
      const nlohmann::json &context,
      const std::unordered_map<std::string, std::string> &environ,
      const envoy::config::core::v3::Metadata *cluster_metadata);

  std::string render(const inja::Template &input);

private:
  // header_value(name)
  nlohmann::json header_callback(const inja::Arguments &args) const;
  nlohmann::json request_header_callback(const inja::Arguments &args) const;
  // extracted_value(name, index)
  nlohmann::json extracted_callback(const inja::Arguments &args) const;
  nlohmann::json dynamic_metadata(const inja::Arguments &args) const;
  nlohmann::json env(const inja::Arguments &args) const;
  nlohmann::json cluster_metadata_callback(const inja::Arguments &args) const;
  nlohmann::json base64_encode_callback(const inja::Arguments &args) const;
  nlohmann::json base64_decode_callback(const inja::Arguments &args) const;
  nlohmann::json substring_callback(const inja::Arguments &args) const;

  inja::Environment env_;
  const Http::RequestOrResponseHeaderMap &header_map_;
  const Http::RequestHeaderMap *request_headers_;
  GetBodyFunc &body_;
  const std::unordered_map<std::string, absl::string_view> &extractions_;
  const nlohmann::json &context_;
  const std::unordered_map<std::string, std::string> &environ_;
  const envoy::config::core::v3::Metadata *cluster_metadata_;
};

class Extractor : Logger::Loggable<Logger::Id::filter> {
public:
  Extractor(const envoy::api::v2::filter::http::Extraction &extractor);
  absl::string_view extract(Http::StreamFilterCallbacks &callbacks,
                            const Http::RequestOrResponseHeaderMap &header_map,
                            GetBodyFunc &body) const;

private:
  absl::string_view extractValue(Http::StreamFilterCallbacks &callbacks,
                                 absl::string_view value) const;

  const Http::LowerCaseString headername_;
  const bool body_;
  const unsigned int group_;
  const std::regex extract_regex_;
};

class InjaTransformer : public Transformer {
public:
  InjaTransformer(const envoy::api::v2::filter::http::TransformationTemplate
                      &transformation);
  ~InjaTransformer();

  void transform(Http::RequestOrResponseHeaderMap &map,
                 Http::RequestHeaderMap *request_headers,
                 Buffer::Instance &body,
                 Http::StreamFilterCallbacks &) const override;
  bool passthrough_body() const override { return passthrough_body_; };

private:
  struct DynamicMetadataValue {
    std::string namespace_;
    std::string key_;
    inja::Template template_;
  };

  bool advanced_templates_{};
  bool passthrough_body_{};
  std::vector<std::pair<std::string, Extractor>> extractors_;
  std::vector<std::pair<Http::LowerCaseString, inja::Template>> headers_;
  std::vector<std::pair<Http::LowerCaseString, inja::Template>> headers_to_append_;
  std::vector<Http::LowerCaseString> headers_to_remove_;
  std::vector<DynamicMetadataValue> dynamic_metadata_;
  std::unordered_map<std::string, std::string> environ_;

  envoy::api::v2::filter::http::TransformationTemplate::RequestBodyParse
      parse_body_behavior_;
  bool ignore_error_on_parse_;

  absl::optional<inja::Template> body_template_;
  bool merged_extractors_to_body_{};
};

} // namespace Transformation
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
