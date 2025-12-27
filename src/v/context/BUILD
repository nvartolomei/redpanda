load("//bazel:build.bzl", "redpanda_cc_library")

redpanda_cc_library(
    name = "context",
    hdrs = ["context.h"],
    defines = select({
        "@//bazel:optimized_build": ["CONTEXT_DEBUG_REF_COUNTING"],
        "//conditions:default": ["CONTEXT_DEBUG_REF_COUNTING"],
    }),
    visibility = ["//visibility:public"],
    deps = [
        "//src/v/base",
        "@seastar",
    ],
)

redpanda_cc_library(
    name = "context_frame",
    srcs = ["tracing.cc"],
    hdrs = [
        "abort_source.h",
        "context_debug.h",
        "context_frame.h",
        "deadline_timer.h",
        "linker.h",
        "tracing.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":context",
        "@abseil-cpp//absl/container:inlined_vector",
        "@fmt",
        "@seastar",
    ],
)
