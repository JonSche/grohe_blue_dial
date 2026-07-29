# Header-only: the DialState/WaterType single source of truth shared by
# app:: (mutates it) and ui:: (renders it), with no dependency on either.
idf_component_register(
    INCLUDE_DIRS "include"
)
