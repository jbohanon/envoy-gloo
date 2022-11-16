REPOSITORY_LOCATIONS = dict(
    # envoy 1.23.2, commit: https://github.com/envoyproxy/envoy/commit/aa86879734f6ead69ec27b56f0f8e8888ad01f3e
    envoy = dict(
        commit = "296fe1d407a920c9e83e273f48bfd85b8a98cd4a",
        remote = "https://github.com/jbohanon/envoy",
    ),
    inja = dict(
        commit = "4c0ee3a46c0bbb279b0849e5a659e52684a37a98",
        remote = "https://github.com/pantor/inja",
    ),
    json = dict(
        commit = "53c3eefa2cf790a7130fed3e13a3be35c2f2ace2",  # v3.7.0
        remote = "https://github.com/nlohmann/json",
    ),
)
