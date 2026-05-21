find_package(protobuf CONFIG REQUIRED)

set(PROTO_FILES log_record.proto)

add_library(logger_proto STATIC ${PROTO_FILES})

add_library(logger::proto ALIAS logger_proto)

target_compile_features(logger_proto PUBLIC cxx_std_17)

# Generated headers land in ${CMAKE_BINARY_DIR}/proto/log_record.pb.h so they
# can be included as "proto/log_record.pb.h" from any downstream target.
protobuf_generate(
    TARGET logger_proto
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
    PROTOC_OUT_DIR ${CMAKE_BINARY_DIR}
)

target_include_directories(logger_proto PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(logger_proto
    PUBLIC protobuf::libprotobuf
)
