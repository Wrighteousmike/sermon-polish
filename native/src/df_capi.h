#pragma once

// Declarations for DeepFilterNet's C API (libDF `capi` feature, MIT/Apache-2.0;
// see libDF/src/capi.rs). Hand-written because the repo's cbindgen header is
// generated at packaging time.

#include <cstddef>

extern "C" {
struct DFState;

// path: DeepFilterNet *_onnx.tar.gz model; atten_lim: maximum attenuation in
// dB (100 = effectively unlimited); log_level may be null.
DFState* df_create(const char* path, float atten_lim, const char* log_level);
size_t df_get_frame_length(DFState* st);  // samples per frame (480 at 48 kHz)
void df_set_atten_lim(DFState* st, float lim_db);
void df_set_post_filter_beta(DFState* st, float beta);
// input/output: df_get_frame_length() floats each. Returns the local SNR.
float df_process_frame(DFState* st, float* input, float* output);
void df_free(DFState* st);
}
