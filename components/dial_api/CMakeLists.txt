# Header-only: the abstract interface app::App implements and
# components/provisioning/'s new local HTTP API calls through -- exists
# purely so the two can depend on one shared, tiny interface without a
# circular CMake dependency (app already REQUIRES provisioning for
# ProvisioningServer itself; provisioning cannot REQUIRES app back). See
# dial_api.hpp's own comment.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES dial_state settings
)
