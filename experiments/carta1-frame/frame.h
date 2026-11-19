#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental only: singleton runtime with exactly two permanent streams.
 * Each buffer must contain 512 floats or 212 bytes respectively. Calls must be
 * serialized on one thread. Inputs must be valid, complete Carta1 frames. */
int carta_frame_encode(int stream, const float* pcm512, unsigned char* frame212);
int carta_frame_decode(int stream, const unsigned char* frame212, float* pcm512);
#ifdef __cplusplus
}
#endif
