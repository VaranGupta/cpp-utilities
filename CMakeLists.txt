add_library(parameters-manager STATIC)
add_library(ParametersManager ALIAS parameters-manager)

target_include_directories(
    parameters-manager
    PUBLIC
        include
)

target_sources(
    parameters-manager
    PRIVATE
        src/parameters-manager.cpp
)

target_compile_definitions(
    parameters-manager
    PRIVATE
        APP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(
    parameters-manager
    PUBLIC
        yaml-cpp::yaml-cpp
)


add_subdirectory(examples)