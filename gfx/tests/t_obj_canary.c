/* t_obj_canary.c - every guest-visible NVN object stays inside the smallest size the titles' SDKs give it (obj.h NVN_MIN_*, docs/architecture/gfx.md
 * "Guest object sizes"). Each object sits in a block whose bytes from NVN_MIN_<type> on hold a canary; its SetDefaults / Initialize and then every entry
 * point that takes the object as a non-const first parameter (the lists below: nvn_protos.h + nvn_protos_ext.h, Finalize last) run, and the canary must
 * survive each call. A layout that outgrows a title's object (Animal Crossing's NVNprogram ends at +0x88: 30e67b4) fails here with the call that wrote.
 * The lists come from t_nvn_canary_gen.py (first parameter a non-const pointer to the family's object; arguments: 1 / small valid objects / zeroed
 * scratch): an entry point added to nvn_protos*.h for one of these families belongs in its list. Skipped: nvnQueuePresentTexture, nvnQueueAcquireTexture
 * (need an acquired window image), nvnSyncInitializeFromFencedGLSync (GL interop, refused). Headless Metal, no game data. */
#include "nvn_harness.h"
#include "obj.h"

#define CANARY 0x100
#define CANARY_BYTE 0xa5
static H h;
static uint8_t g_scr[0x10000], g_out[0x10000];
static NVNtexture *g_tex, *g_texarr[8];
static NVNtextureView *g_view, *g_viewarr[8];
static NVNsync *g_sync;
static NVNevent *g_event;
static NVNtexturePool *g_tpool;
static NVNsamplerPool *g_spool;
static NVNprogram *g_prog;
static NVNwindow *g_win;
static NVNcommandHandle g_handle;
static NVNbufferRange g_ranges[16];
static NVNcopyRegion g_region = {0, 0, 0, 4, 4, 1};
static NVNdrawTextureRegion g_dtr = {0, 0, 1, 1};
static NVNshaderData g_sd[2];
static NVNbufferAddress g_addr;
static NVNblendState *g_blend; static NVNchannelMaskState *g_chmask; static NVNcolorState *g_color; static NVNdepthStencilState *g_ds;
static NVNmultisampleState *g_ms; static NVNpolygonState *g_poly; static NVNvertexAttribState *g_va; static NVNvertexStreamState *g_vs;
static NVNdeviceBuilder *g_db; static NVNqueueBuilder *g_qb; static NVNeventBuilder *g_eb; static NVNmemoryPoolBuilder *g_pb;
static NVNbufferBuilder *g_bb; static NVNtextureBuilder *g_tb; static NVNsamplerBuilder *g_sb; static NVNwindowBuilder *g_wb;

/* a block for one object: zeroed up to `min`, canary after it */
static uint8_t *block(size_t min) {
    uint8_t *b = aligned_alloc(256, (min + CANARY + 255) & ~(size_t)255);
    memset(b, 0, min);
    memset(b + min, CANARY_BYTE, CANARY);
    return b;
}
static int g_calls;
static void check(const uint8_t *o, size_t min, const char *type, const char *fn) {
    g_calls++;
    for (size_t i = 0; i < CANARY; i++)
        if (o[min + i] != CANARY_BYTE) {
            CHECK(0, "%s wrote byte +%#zx of a %s (smallest size %#zx)", fn, min + i, type, min);
            memset((uint8_t *)o + min, CANARY_BYTE, CANARY);
            return;
        }
}
#define X(fn, ...) do { L(fn)(__VA_ARGS__); check((const uint8_t *)o, min, type, #fn); } while (0);
#define FAMILY(T, NAME)                                                                                                                       \
    do {                                                                                                                                      \
        const size_t min = NVN_MIN_##T; const char *type = #T;                                                                                \
        T *o = (T *)block(min);                                                                                                               \
        int fails = g_fail;                                                                                                                   \
        CALLS_##NAME(X)                                                                                                                       \
        printf("%-22s smallest %#6zx: %s\n", #T, min, g_fail == fails ? "canary intact" : "OVERRUN");                                         \
        free(o);                                                                                                                              \
    } while (0)

#define CALLS_DeviceBuilder(X) \
    X(nvnDeviceBuilderSetDefaults, o) \
    X(nvnDeviceBuilderSetFlags, o, 1) \

#define CALLS_Device(X) \
    X(nvnDeviceInitialize, o, g_db) \
    X(nvnDeviceSetDebugLabel, o, "canary") \
    X(nvnDeviceSetIntermediateShaderCache, o, 1) \
    X(nvnDeviceInstallDebugCallback, o, NULL, g_scr, 1) \
    X(nvnDeviceSetWindowOriginMode, o, (NVNwindowOriginMode)1) \
    X(nvnDeviceSetDepthMode, o, (NVNdepthMode)1) \
    X(nvnDeviceRegisterFastClearColor, o, (const float *)g_scr, (NVNformat)1) \
    X(nvnDeviceRegisterFastClearColori, o, (const int *)g_scr, (NVNformat)1) \
    X(nvnDeviceRegisterFastClearColorui, o, (const uint32_t *)g_scr, (NVNformat)1) \
    X(nvnDeviceRegisterFastClearDepth, o, 1.0f) \
    X(nvnDeviceApplyDeferredFinalizes, o, 1) \
    X(nvnDeviceFinalizeCommandHandle, o, 0) \
    X(nvnDeviceSetAutomaticCheckpointsEnable, o, 1) \
    X(nvnDeviceSetAutomaticCheckpointsSamplingInterval, o, 1) \
    X(nvnDeviceSetAutomaticCheckpointsByDebugGroup, o, 1) \
    X(nvnDeviceSetCheckpointCallStacksEnable, o, 1) \
    X(nvnDeviceSetCheckpointMaxCallStacksDepth, o, 1) \
    X(nvnDeviceSetAutomaticCheckpointsMask, o, 1) \
    X(nvnDeviceWaitForError, o, 0) \
    X(nvnDeviceFinalize, o) \

#define CALLS_QueueBuilder(X) \
    X(nvnQueueBuilderSetDevice, o, h.dev) \
    X(nvnQueueBuilderSetDefaults, o) \
    X(nvnQueueBuilderSetFlags, o, 1) \
    X(nvnQueueBuilderSetCommandMemorySize, o, 0x1000) \
    X(nvnQueueBuilderSetComputeMemorySize, o, 0x1000) \
    X(nvnQueueBuilderSetControlMemorySize, o, 0x1000) \
    X(nvnQueueBuilderSetQueueMemory, o, g_scr, 0x1000) \
    X(nvnQueueBuilderSetCommandFlushThreshold, o, 0x1000) \
    X(nvnQueueBuilderSetQueuePriority, o, 1) \

#define CALLS_Queue(X) \
    X(nvnQueueInitialize, o, g_qb) \
    X(nvnQueueGetError, o, (NVNqueueErrorInfo *)g_out) \
    X(nvnQueueGetTotalCommandMemoryUsed, o) \
    X(nvnQueueGetTotalControlMemoryUsed, o) \
    X(nvnQueueGetTotalComputeMemoryUsed, o) \
    X(nvnQueueResetMemoryUsageCounts, o) \
    X(nvnQueueSetDebugLabel, o, "canary") \
    X(nvnQueueSubmitCommands, o, 1, &g_handle) \
    X(nvnQueueFlush, o) \
    X(nvnQueueFinish, o) \
    X(nvnQueueFenceSync, o, g_sync, (NVNsyncCondition)1, 1) \
    X(nvnQueueWaitSync, o, g_sync) \
    X(nvnQueueFinalize, o) \

#define CALLS_Sync(X) \
    X(nvnSyncInitialize, o, h.dev) \
    X(nvnSyncSetDebugLabel, o, "canary") \
    X(nvnSyncFinalize, o) \

#define CALLS_EventBuilder(X) \
    X(nvnEventBuilderSetDefaults, o) \
    X(nvnEventBuilderSetStorage, o, h.pool, 0) \

#define CALLS_Event(X) \
    X(nvnEventInitialize, o, g_eb) \
    X(nvnEventSignal, o, (NVNeventSignalMode)1, 1) \
    X(nvnEventFinalize, o) \

#define CALLS_MemoryPoolBuilder(X) \
    X(nvnMemoryPoolBuilderSetDevice, o, h.dev) \
    X(nvnMemoryPoolBuilderSetDefaults, o) \
    X(nvnMemoryPoolBuilderSetStorage, o, g_scr, 0x1000) \
    X(nvnMemoryPoolBuilderSetFlags, o, 1) \

#define CALLS_MemoryPool(X) \
    X(nvnMemoryPoolInitialize, o, g_pb) \
    X(nvnMemoryPoolSetDebugLabel, o, "canary") \
    X(nvnMemoryPoolMapVirtual, o, 1, (const NVNmappingRequest *)g_scr) \
    X(nvnMemoryPoolFinalize, o) \

#define CALLS_BufferBuilder(X) \
    X(nvnBufferBuilderSetDevice, o, h.dev) \
    X(nvnBufferBuilderSetDefaults, o) \
    X(nvnBufferBuilderSetStorage, o, h.pool, 0, 0x1000) \

#define CALLS_Buffer(X) \
    X(nvnBufferInitialize, o, g_bb) \
    X(nvnBufferSetDebugLabel, o, "canary") \
    X(nvnBufferFinalize, o) \

#define CALLS_TextureBuilder(X) \
    X(nvnTextureBuilderSetDevice, o, h.dev) \
    X(nvnTextureBuilderSetDefaults, o) \
    X(nvnTextureBuilderSetFlags, o, 1) \
    X(nvnTextureBuilderSetTarget, o, (NVNtextureTarget)1) \
    X(nvnTextureBuilderSetWidth, o, 1) \
    X(nvnTextureBuilderSetHeight, o, 1) \
    X(nvnTextureBuilderSetDepth, o, 1) \
    X(nvnTextureBuilderSetSize1D, o, 1) \
    X(nvnTextureBuilderSetSize2D, o, 1, 1) \
    X(nvnTextureBuilderSetSize3D, o, 1, 1, 1) \
    X(nvnTextureBuilderSetLevels, o, 1) \
    X(nvnTextureBuilderSetFormat, o, (NVNformat)1) \
    X(nvnTextureBuilderSetSamples, o, 1) \
    X(nvnTextureBuilderSetSwizzle, o, (NVNtextureSwizzle)1, (NVNtextureSwizzle)1, (NVNtextureSwizzle)1, (NVNtextureSwizzle)1) \
    X(nvnTextureBuilderSetDepthStencilMode, o, (NVNtextureDepthStencilMode)1) \
    X(nvnTextureBuilderSetStorage, o, h.pool, 0) \
    X(nvnTextureBuilderSetPackagedTextureData, o, g_scr) \
    X(nvnTextureBuilderSetPackagedTextureLayout, o, (const NVNpackagedTextureLayout *)g_scr) \
    X(nvnTextureBuilderSetStride, o, 0) \
    X(nvnTextureBuilderSetGLTextureName, o, 1) \

#define CALLS_TextureView(X) \
    X(nvnTextureViewSetDefaults, o) \
    X(nvnTextureViewSetLevels, o, 1, 1) \
    X(nvnTextureViewSetLayers, o, 1, 1) \
    X(nvnTextureViewSetFormat, o, (NVNformat)1) \
    X(nvnTextureViewSetSwizzle, o, (NVNtextureSwizzle)1, (NVNtextureSwizzle)1, (NVNtextureSwizzle)1, (NVNtextureSwizzle)1) \
    X(nvnTextureViewSetDepthStencilMode, o, (NVNtextureDepthStencilMode)1) \
    X(nvnTextureViewSetTarget, o, (NVNtextureTarget)1) \

#define CALLS_Texture(X) \
    X(nvnTextureInitialize, o, g_tb) \
    X(nvnTextureSetDebugLabel, o, "canary") \
    X(nvnTextureFinalize, o) \

#define CALLS_SamplerBuilder(X) \
    X(nvnSamplerBuilderSetDevice, o, h.dev) \
    X(nvnSamplerBuilderSetDefaults, o) \
    X(nvnSamplerBuilderSetMinMagFilter, o, (NVNminFilter)1, (NVNmagFilter)1) \
    X(nvnSamplerBuilderSetWrapMode, o, (NVNwrapMode)1, (NVNwrapMode)1, (NVNwrapMode)1) \
    X(nvnSamplerBuilderSetLodClamp, o, 1.0f, 1.0f) \
    X(nvnSamplerBuilderSetLodBias, o, 1.0f) \
    X(nvnSamplerBuilderSetCompare, o, (NVNcompareMode)1, (NVNcompareFunc)1) \
    X(nvnSamplerBuilderSetBorderColor, o, (const float *)g_scr) \
    X(nvnSamplerBuilderSetBorderColori, o, (const int *)g_scr) \
    X(nvnSamplerBuilderSetBorderColorui, o, (const uint32_t *)g_scr) \
    X(nvnSamplerBuilderSetMaxAnisotropy, o, 1.0f) \
    X(nvnSamplerBuilderSetReductionFilter, o, (NVNsamplerReduction)1) \
    X(nvnSamplerBuilderSetLodSnap, o, 1.0f) \

#define CALLS_Sampler(X) \
    X(nvnSamplerInitialize, o, g_sb) \
    X(nvnSamplerSetDebugLabel, o, "canary") \
    X(nvnSamplerFinalize, o) \

#define CALLS_TexturePool(X) \
    X(nvnTexturePoolInitialize, o, h.pool, 0, 1) \
    X(nvnTexturePoolSetDebugLabel, o, "canary") \
    X(nvnTexturePoolFinalize, o) \

#define CALLS_SamplerPool(X) \
    X(nvnSamplerPoolInitialize, o, h.pool, 0, 1) \
    X(nvnSamplerPoolSetDebugLabel, o, "canary") \
    X(nvnSamplerPoolFinalize, o) \

#define CALLS_Program(X) \
    X(nvnProgramInitialize, o, h.dev) \
    X(nvnProgramSetDebugLabel, o, "canary") \
    X(nvnProgramSetShaders, o, 2, g_sd) \
    X(nvnProgramSetSubroutineLinkage, o, 1, (const NVNsubroutineLinkageMapPtr *)g_scr) \
    X(nvnProgramSetShadersExt, o, 2, g_sd) \
    X(nvnProgramSetSampleShading, o, 1, 1.0f) \
    X(nvnProgramFinalize, o) \

#define CALLS_WindowBuilder(X) \
    X(nvnWindowBuilderSetDevice, o, h.dev) \
    X(nvnWindowBuilderSetDefaults, o) \
    X(nvnWindowBuilderSetNativeWindow, o, NULL) \
    X(nvnWindowBuilderSetTextures, o, 2, g_texarr) \
    X(nvnWindowBuilderSetPresentInterval, o, 1) \
    X(nvnWindowBuilderSetNumActiveTextures, o, 1) \

#define CALLS_Window(X) \
    X(nvnWindowInitialize, o, g_wb) \
    X(nvnWindowSetDebugLabel, o, "canary") \
    X(nvnWindowAcquireTexture, o, g_sync, (int *)g_out) \
    X(nvnWindowSetPresentInterval, o, 1) \
    X(nvnWindowSetCrop, o, 1, 1, 1, 1) \
    X(nvnWindowSetNumActiveTextures, o, 1) \
    X(nvnWindowFinalize, o) \

#define CALLS_CommandBuffer(X) \
    X(nvnCommandBufferInitialize, o, h.dev) \
    X(nvnCommandBufferAddCommandMemory, o, h.pool, 0, 0x1000) \
    X(nvnCommandBufferAddControlMemory, o, g_scr, 0x1000) \
    X(nvnCommandBufferBeginRecording, o) \
    X(nvnCommandBufferSetDebugLabel, o, "canary") \
    X(nvnCommandBufferSetMemoryCallback, o, NULL) \
    X(nvnCommandBufferSetMemoryCallbackData, o, g_scr) \
    X(nvnCommandBufferCallCommands, o, 1, &g_handle) \
    X(nvnCommandBufferCopyCommands, o, 1, &g_handle) \
    X(nvnCommandBufferBindBlendState, o, g_blend) \
    X(nvnCommandBufferBindChannelMaskState, o, g_chmask) \
    X(nvnCommandBufferBindColorState, o, g_color) \
    X(nvnCommandBufferBindMultisampleState, o, g_ms) \
    X(nvnCommandBufferBindPolygonState, o, g_poly) \
    X(nvnCommandBufferBindDepthStencilState, o, g_ds) \
    X(nvnCommandBufferBindVertexAttribState, o, 1, g_va) \
    X(nvnCommandBufferBindVertexStreamState, o, 1, g_vs) \
    X(nvnCommandBufferBindProgram, o, g_prog, 1) \
    X(nvnCommandBufferBindVertexBuffer, o, 1, g_addr, 0x1000) \
    X(nvnCommandBufferBindVertexBuffers, o, 1, 1, g_ranges) \
    X(nvnCommandBufferBindUniformBuffer, o, (NVNshaderStage)1, 1, g_addr, 0x1000) \
    X(nvnCommandBufferBindUniformBuffers, o, (NVNshaderStage)1, 1, 1, g_ranges) \
    X(nvnCommandBufferBindTransformFeedbackBuffer, o, 1, g_addr, 0x1000) \
    X(nvnCommandBufferBindTransformFeedbackBuffers, o, 1, 1, g_ranges) \
    X(nvnCommandBufferBindStorageBuffer, o, (NVNshaderStage)1, 1, g_addr, 0x1000) \
    X(nvnCommandBufferBindStorageBuffers, o, (NVNshaderStage)1, 1, 1, g_ranges) \
    X(nvnCommandBufferBindTexture, o, (NVNshaderStage)1, 1, 0) \
    X(nvnCommandBufferBindTextures, o, (NVNshaderStage)1, 1, 1, (const NVNtextureHandle *)g_scr) \
    X(nvnCommandBufferBindImage, o, (NVNshaderStage)1, 1, 0) \
    X(nvnCommandBufferBindImages, o, (NVNshaderStage)1, 1, 1, (const NVNimageHandle *)g_scr) \
    X(nvnCommandBufferSetPatchSize, o, 1) \
    X(nvnCommandBufferSetInnerTessellationLevels, o, (const float *)g_scr) \
    X(nvnCommandBufferSetOuterTessellationLevels, o, (const float *)g_scr) \
    X(nvnCommandBufferSetPrimitiveRestart, o, 1, 1) \
    X(nvnCommandBufferBeginTransformFeedback, o, g_addr) \
    X(nvnCommandBufferEndTransformFeedback, o, g_addr) \
    X(nvnCommandBufferPauseTransformFeedback, o, g_addr) \
    X(nvnCommandBufferResumeTransformFeedback, o, g_addr) \
    X(nvnCommandBufferDrawTransformFeedback, o, (NVNdrawPrimitive)1, g_addr) \
    X(nvnCommandBufferDrawArrays, o, (NVNdrawPrimitive)1, 1, 1) \
    X(nvnCommandBufferDrawElements, o, (NVNdrawPrimitive)1, (NVNindexType)1, 1, g_addr) \
    X(nvnCommandBufferDrawElementsBaseVertex, o, (NVNdrawPrimitive)1, (NVNindexType)1, 1, g_addr, 1) \
    X(nvnCommandBufferDrawArraysInstanced, o, (NVNdrawPrimitive)1, 1, 1, 1, 1) \
    X(nvnCommandBufferDrawElementsInstanced, o, (NVNdrawPrimitive)1, (NVNindexType)1, 1, g_addr, 1, 1, 1) \
    X(nvnCommandBufferDrawArraysIndirect, o, (NVNdrawPrimitive)1, g_addr) \
    X(nvnCommandBufferDrawElementsIndirect, o, (NVNdrawPrimitive)1, (NVNindexType)1, g_addr, g_addr) \
    X(nvnCommandBufferMultiDrawArraysIndirectCount, o, (NVNdrawPrimitive)1, g_addr, g_addr, 1, 0) \
    X(nvnCommandBufferMultiDrawElementsIndirectCount, o, (NVNdrawPrimitive)1, (NVNindexType)1, g_addr, g_addr, g_addr, 1, 0) \
    X(nvnCommandBufferClearColor, o, 1, (const float *)g_scr, 1) \
    X(nvnCommandBufferClearColori, o, 1, (const int *)g_scr, 1) \
    X(nvnCommandBufferClearColorui, o, 1, (const uint32_t *)g_scr, 1) \
    X(nvnCommandBufferClearDepthStencil, o, 1.0f, 1, 1, 1) \
    X(nvnCommandBufferDispatchCompute, o, 1, 1, 1) \
    X(nvnCommandBufferDispatchComputeIndirect, o, g_addr) \
    X(nvnCommandBufferSetViewport, o, 1, 1, 1, 1) \
    X(nvnCommandBufferSetViewports, o, 1, 1, (const float *)g_scr) \
    X(nvnCommandBufferSetViewportSwizzles, o, 1, 1, (const NVNviewportSwizzle *)g_scr) \
    X(nvnCommandBufferSetScissor, o, 1, 1, 1, 1) \
    X(nvnCommandBufferSetScissors, o, 1, 1, (const int *)g_scr) \
    X(nvnCommandBufferSetDepthRange, o, 1.0f, 1.0f) \
    X(nvnCommandBufferSetDepthBounds, o, 1, 1.0f, 1.0f) \
    X(nvnCommandBufferSetDepthRanges, o, 1, 1, (const float *)g_scr) \
    X(nvnCommandBufferSetTiledCacheAction, o, (NVNtiledCacheAction)1) \
    X(nvnCommandBufferSetTiledCacheTileSize, o, 1, 1) \
    X(nvnCommandBufferBindSeparateTexture, o, (NVNshaderStage)1, 1, (NVNseparateTextureHandle){0}) \
    X(nvnCommandBufferBindSeparateSampler, o, (NVNshaderStage)1, 1, (NVNseparateSamplerHandle){0}) \
    X(nvnCommandBufferBindSeparateTextures, o, (NVNshaderStage)1, 1, 1, (const NVNseparateTextureHandle *)g_scr) \
    X(nvnCommandBufferBindSeparateSamplers, o, (NVNshaderStage)1, 1, 1, (const NVNseparateSamplerHandle *)g_scr) \
    X(nvnCommandBufferSetStencilValueMask, o, (NVNface)1, 1) \
    X(nvnCommandBufferSetStencilMask, o, (NVNface)1, 1) \
    X(nvnCommandBufferSetStencilRef, o, (NVNface)1, 1) \
    X(nvnCommandBufferSetBlendColor, o, (const float *)g_scr) \
    X(nvnCommandBufferSetPointSize, o, 1.0f) \
    X(nvnCommandBufferSetLineWidth, o, 1.0f) \
    X(nvnCommandBufferSetPolygonOffsetClamp, o, 1.0f, 1.0f, 1.0f) \
    X(nvnCommandBufferSetAlphaRef, o, 1.0f) \
    X(nvnCommandBufferSetSampleMask, o, 1) \
    X(nvnCommandBufferSetRasterizerDiscard, o, 1) \
    X(nvnCommandBufferSetDepthClamp, o, 1) \
    X(nvnCommandBufferSetConservativeRasterEnable, o, 1) \
    X(nvnCommandBufferSetConservativeRasterDilate, o, 1.0f) \
    X(nvnCommandBufferSetSubpixelPrecisionBias, o, 1, 1) \
    X(nvnCommandBufferCopyBufferToTexture, o, g_addr, g_tex, g_view, &g_region, 1) \
    X(nvnCommandBufferCopyTextureToBuffer, o, g_tex, g_view, &g_region, g_addr, 1) \
    X(nvnCommandBufferCopyTextureToTexture, o, g_tex, g_view, &g_region, g_tex, g_view, &g_region, 1) \
    X(nvnCommandBufferCopyBufferToBuffer, o, g_addr, g_addr, 0x1000, 1) \
    X(nvnCommandBufferClearBuffer, o, g_addr, 0x1000, 1) \
    X(nvnCommandBufferClearTexture, o, g_tex, g_view, &g_region, (const float *)g_scr, 1) \
    X(nvnCommandBufferClearTexturei, o, g_tex, g_view, &g_region, (const int *)g_scr, 1) \
    X(nvnCommandBufferClearTextureui, o, g_tex, g_view, &g_region, (const uint32_t *)g_scr, 1) \
    X(nvnCommandBufferUpdateUniformBuffer, o, g_addr, 0x1000, 0, 0x1000, g_scr) \
    X(nvnCommandBufferReportCounter, o, (NVNcounterType)1, g_addr) \
    X(nvnCommandBufferResetCounter, o, (NVNcounterType)1) \
    X(nvnCommandBufferReportValue, o, 1, g_addr) \
    X(nvnCommandBufferSetRenderEnable, o, 1) \
    X(nvnCommandBufferSetRenderEnableConditional, o, (NVNconditionalRenderMode)1, g_addr) \
    X(nvnCommandBufferSetRenderTargets, o, 1, (const NVNtexture *const *)g_texarr, (const NVNtextureView *const *)g_viewarr, g_tex, g_view) \
    X(nvnCommandBufferDiscardColor, o, 1) \
    X(nvnCommandBufferDiscardDepthStencil, o) \
    X(nvnCommandBufferDownsample, o, g_tex, g_tex) \
    X(nvnCommandBufferTiledDownsample, o, g_tex, g_tex) \
    X(nvnCommandBufferDownsampleTextureView, o, g_tex, g_view, g_tex, g_view) \
    X(nvnCommandBufferTiledDownsampleTextureView, o, g_tex, g_view, g_tex, g_view) \
    X(nvnCommandBufferBarrier, o, 1) \
    X(nvnCommandBufferWaitSync, o, g_sync) \
    X(nvnCommandBufferFenceSync, o, g_sync, (NVNsyncCondition)1, 1) \
    X(nvnCommandBufferSetTexturePool, o, g_tpool) \
    X(nvnCommandBufferSetSamplerPool, o, g_spool) \
    X(nvnCommandBufferSetShaderScratchMemory, o, h.pool, 0, 0x1000) \
    X(nvnCommandBufferSaveZCullData, o, g_addr, 0x1000) \
    X(nvnCommandBufferRestoreZCullData, o, g_addr, 0x1000) \
    X(nvnCommandBufferSetCopyRowStride, o, 0) \
    X(nvnCommandBufferSetCopyImageStride, o, 0) \
    X(nvnCommandBufferDrawTexture, o, 0, &g_dtr, &g_dtr) \
    X(nvnCommandBufferSetProgramSubroutines, o, g_prog, (NVNshaderStage)1, 1, 1, (const int *)g_scr) \
    X(nvnCommandBufferBindCoverageModulationTable, o, (const float *)g_scr) \
    X(nvnCommandBufferResolveDepthBuffer, o) \
    X(nvnCommandBufferPushDebugGroupStatic, o, 1, "canary") \
    X(nvnCommandBufferPushDebugGroupDynamic, o, 1, "canary") \
    X(nvnCommandBufferPushDebugGroup, o, 1, "canary") \
    X(nvnCommandBufferPopDebugGroup, o) \
    X(nvnCommandBufferPopDebugGroupId, o, 1) \
    X(nvnCommandBufferInsertDebugMarkerStatic, o, 1, "canary") \
    X(nvnCommandBufferInsertDebugMarkerDynamic, o, 1, "canary") \
    X(nvnCommandBufferInsertDebugMarker, o, "canary") \
    X(nvnCommandBufferWaitEvent, o, g_event, (NVNeventWaitMode)1, 1) \
    X(nvnCommandBufferSignalEvent, o, g_event, (NVNeventSignalMode)1, (NVNeventSignalLocation)1, 1, 1) \
    X(nvnCommandBufferSetCommandMemoryCallbackEnabled, o, 1) \
    X(nvnCommandBufferSetColorReductionEnable, o, 1) \
    X(nvnCommandBufferSetColorReductionThresholds, o, 1, 1) \
    X(nvnCommandBufferSetStencilCullCriteria, o, 1, 1, 1) \
    X(nvnCommandBufferInsertCheckpointStatic, o, 1, "canary") \
    X(nvnCommandBufferInsertCheckpointDynamic, o, 1, "canary") \
    X(nvnCommandBufferBindUniformBuffersDeferred, o, (NVNshaderStage)1, 1, 1, g_ranges) \
    X(nvnCommandBufferBindStorageBuffersDeferred, o, (NVNshaderStage)1, 1, 1, g_ranges) \
    X(nvnCommandBufferBindTexturesDeferred, o, (NVNshaderStage)1, 1, 1, (const NVNtextureHandle *)g_scr) \
    X(nvnCommandBufferBindImagesDeferred, o, (NVNshaderStage)1, 1, 1, (const NVNimageHandle *)g_scr) \
    X(nvnCommandBufferBindSeparateTexturesDeferred, o, (NVNshaderStage)1, 1, 1, (const NVNseparateTextureHandle *)g_scr) \
    X(nvnCommandBufferBindSeparateSamplersDeferred, o, (NVNshaderStage)1, 1, 1, (const NVNseparateSamplerHandle *)g_scr) \
    X(nvnCommandBufferOverrideSubtileSize, o, 1, 1) \
    X(nvnCommandBufferSetControlMemoryCallbackEnabled, o, 1) \
    X(nvnCommandBufferSetProvokingVertexMode, o, 1) \
    X(nvnCommandBufferSetZCullZF32CompressionEnable, o, 1) \
    X(nvnCommandBufferEndRecording, o) \
    X(nvnCommandBufferFinalize, o) \

#define CALLS_BlendState(X) \
    X(nvnBlendStateSetDefaults, o) \
    X(nvnBlendStateSetBlendTarget, o, 1) \
    X(nvnBlendStateSetBlendFunc, o, (NVNblendFunc)1, (NVNblendFunc)1, (NVNblendFunc)1, (NVNblendFunc)1) \
    X(nvnBlendStateSetBlendEquation, o, (NVNblendEquation)1, (NVNblendEquation)1) \
    X(nvnBlendStateSetAdvancedMode, o, 1) \
    X(nvnBlendStateSetAdvancedOverlap, o, 1) \
    X(nvnBlendStateSetAdvancedPremultipliedSrc, o, 1) \
    X(nvnBlendStateSetAdvancedNormalizedDst, o, 1) \

#define CALLS_ColorState(X) \
    X(nvnColorStateSetDefaults, o) \
    X(nvnColorStateSetBlendEnable, o, 1, 1) \
    X(nvnColorStateSetLogicOp, o, 1) \
    X(nvnColorStateSetAlphaTest, o, 1) \

#define CALLS_ChannelMaskState(X) \
    X(nvnChannelMaskStateSetDefaults, o) \
    X(nvnChannelMaskStateSetChannelMask, o, 1, 1, 1, 1, 1) \

#define CALLS_MultisampleState(X) \
    X(nvnMultisampleStateSetDefaults, o) \
    X(nvnMultisampleStateSetMultisampleEnable, o, 1) \
    X(nvnMultisampleStateSetSamples, o, 1) \
    X(nvnMultisampleStateSetAlphaToCoverageEnable, o, 1) \
    X(nvnMultisampleStateSetAlphaToCoverageDither, o, 1) \
    X(nvnMultisampleStateSetRasterSamples, o, 1) \
    X(nvnMultisampleStateGetRasterSamples, o) \
    X(nvnMultisampleStateSetCoverageModulationMode, o, (NVNcoverageModulationMode)1) \
    X(nvnMultisampleStateSetCoverageToColorEnable, o, 1) \
    X(nvnMultisampleStateSetCoverageToColorOutput, o, 1) \
    X(nvnMultisampleStateSetSampleLocationsEnable, o, 1) \
    X(nvnMultisampleStateSetSampleLocationsGridEnable, o, 1) \
    X(nvnMultisampleStateSetSampleLocations, o, 1, 1, (const float *)g_scr) \

#define CALLS_PolygonState(X) \
    X(nvnPolygonStateSetDefaults, o) \
    X(nvnPolygonStateSetCullFace, o, (NVNface)1) \
    X(nvnPolygonStateSetFrontFace, o, (NVNfrontFace)1) \
    X(nvnPolygonStateSetPolygonMode, o, (NVNpolygonMode)1) \
    X(nvnPolygonStateSetPolygonOffsetEnables, o, 1) \

#define CALLS_DepthStencilState(X) \
    X(nvnDepthStencilStateSetDefaults, o) \
    X(nvnDepthStencilStateSetDepthTestEnable, o, 1) \
    X(nvnDepthStencilStateSetDepthWriteEnable, o, 1) \
    X(nvnDepthStencilStateSetDepthFunc, o, (NVNdepthFunc)1) \
    X(nvnDepthStencilStateSetStencilTestEnable, o, 1) \
    X(nvnDepthStencilStateSetStencilFunc, o, (NVNface)1, (NVNstencilFunc)1) \
    X(nvnDepthStencilStateSetStencilOp, o, (NVNface)1, (NVNstencilOp)1, (NVNstencilOp)1, (NVNstencilOp)1) \

#define CALLS_VertexAttribState(X) \
    X(nvnVertexAttribStateSetDefaults, o) \
    X(nvnVertexAttribStateSetFormat, o, (NVNformat)1, 0) \
    X(nvnVertexAttribStateSetStreamIndex, o, 1) \

#define CALLS_VertexStreamState(X) \
    X(nvnVertexStreamStateSetDefaults, o) \
    X(nvnVertexStreamStateSetStride, o, 0) \
    X(nvnVertexStreamStateSetDivisor, o, 1) \

/* real objects the calls take as arguments */
static void setup(void) {
    g_db = calloc(1, sizeof(NVNdeviceBuilder)); L(nvnDeviceBuilderSetDefaults)(g_db);
    g_qb = calloc(1, sizeof(NVNqueueBuilder)); L(nvnQueueBuilderSetDevice)(g_qb, h.dev); L(nvnQueueBuilderSetDefaults)(g_qb);
    g_eb = calloc(1, sizeof(NVNeventBuilder)); L(nvnEventBuilderSetDefaults)(g_eb); L(nvnEventBuilderSetStorage)(g_eb, h.pool, 0x3f0000);
    static uint8_t pool2[0x10000] __attribute__((aligned(0x1000)));
    g_pb = calloc(1, sizeof(NVNmemoryPoolBuilder)); L(nvnMemoryPoolBuilderSetDevice)(g_pb, h.dev); L(nvnMemoryPoolBuilderSetDefaults)(g_pb);
    L(nvnMemoryPoolBuilderSetStorage)(g_pb, pool2, sizeof pool2);
    L(nvnMemoryPoolBuilderSetFlags)(g_pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    g_bb = calloc(1, sizeof(NVNbufferBuilder)); L(nvnBufferBuilderSetDevice)(g_bb, h.dev); L(nvnBufferBuilderSetDefaults)(g_bb);
    L(nvnBufferBuilderSetStorage)(g_bb, h.pool, 0x200000, 0x1000);
    g_tb = calloc(1, sizeof(NVNtextureBuilder)); L(nvnTextureBuilderSetDevice)(g_tb, h.dev); L(nvnTextureBuilderSetDefaults)(g_tb);
    L(nvnTextureBuilderSetTarget)(g_tb, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetFormat)(g_tb, NVN_FORMAT_RGBA8); L(nvnTextureBuilderSetSize2D)(g_tb, 4, 4);
    L(nvnTextureBuilderSetStorage)(g_tb, h.pool, 0x100000);
    g_sb = calloc(1, sizeof(NVNsamplerBuilder)); L(nvnSamplerBuilderSetDevice)(g_sb, h.dev); L(nvnSamplerBuilderSetDefaults)(g_sb);
    g_tex = calloc(1, sizeof(NVNtexture)); CHECK(L(nvnTextureInitialize)(g_tex, g_tb), "texture");
    for (int i = 0; i < 8; i++) g_texarr[i] = g_tex;
    g_view = calloc(1, sizeof(NVNtextureView)); L(nvnTextureViewSetDefaults)(g_view);
    for (int i = 0; i < 8; i++) g_viewarr[i] = g_view;
    g_wb = calloc(1, sizeof(NVNwindowBuilder)); L(nvnWindowBuilderSetDevice)(g_wb, h.dev); L(nvnWindowBuilderSetDefaults)(g_wb);
    NVNtexture *wt[2];
    for (int i = 0; i < 2; i++) wt[i] = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 0x120000 + 0x1000 * (size_t)i, NVN_TEXTURE_FLAGS_DISPLAY);
    L(nvnWindowBuilderSetTextures)(g_wb, 2, wt);
    g_sync = calloc(1, sizeof(NVNsync)); CHECK(L(nvnSyncInitialize)(g_sync, h.dev), "sync");
    g_event = calloc(1, sizeof(NVNevent)); CHECK(L(nvnEventInitialize)(g_event, g_eb), "event");
    g_tpool = calloc(1, sizeof(NVNtexturePool)); CHECK(L(nvnTexturePoolInitialize)(g_tpool, h.pool, 0x380000, 64), "texture pool");
    g_spool = calloc(1, sizeof(NVNsamplerPool)); CHECK(L(nvnSamplerPoolInitialize)(g_spool, h.pool, 0x390000, 64), "sampler pool");
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(0, 0), ins_mov32i(1, 0x3f800000), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit()};
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + 0x300000, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + 0x302000, fctl, 1, fs_i, 5, 0xf);
    g_sd[0] = (NVNshaderData){(NVNbufferAddress)(uintptr_t)(h.mem + 0x300000), vctl};
    g_sd[1] = (NVNshaderData){(NVNbufferAddress)(uintptr_t)(h.mem + 0x302000), fctl};
    g_prog = calloc(1, sizeof(NVNprogram)); CHECK(L(nvnProgramInitialize)(g_prog, h.dev), "program"); CHECK(L(nvnProgramSetShaders)(g_prog, 2, g_sd), "shaders");
    g_addr = (NVNbufferAddress)(uintptr_t)(h.mem + 0x200000);
    for (int i = 0; i < 16; i++) g_ranges[i] = (NVNbufferRange){g_addr, 0x100};
    g_blend = calloc(1, 8); L(nvnBlendStateSetDefaults)(g_blend);
    g_chmask = calloc(1, 8); L(nvnChannelMaskStateSetDefaults)(g_chmask);
    g_color = calloc(1, 8); L(nvnColorStateSetDefaults)(g_color);
    g_ds = calloc(1, 8); L(nvnDepthStencilStateSetDefaults)(g_ds);
    g_ms = calloc(1, sizeof(NVNmultisampleState)); L(nvnMultisampleStateSetDefaults)(g_ms);
    g_poly = calloc(1, 8); L(nvnPolygonStateSetDefaults)(g_poly);
    g_va = calloc(1, 8); L(nvnVertexAttribStateSetDefaults)(g_va);
    g_vs = calloc(1, 8); L(nvnVertexStreamStateSetDefaults)(g_vs);
    L(nvnCommandBufferBeginRecording)(h.cb);
    g_handle = L(nvnCommandBufferEndRecording)(h.cb);
    (void)g_win;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, 8, 8, NULL)) return 1;
    setup();
    FAMILY(NVNdeviceBuilder, DeviceBuilder);
    FAMILY(NVNqueueBuilder, QueueBuilder);
    FAMILY(NVNqueue, Queue);
    FAMILY(NVNsync, Sync);
    FAMILY(NVNeventBuilder, EventBuilder);
    FAMILY(NVNevent, Event);
    FAMILY(NVNmemoryPoolBuilder, MemoryPoolBuilder);
    FAMILY(NVNmemoryPool, MemoryPool);
    FAMILY(NVNbufferBuilder, BufferBuilder);
    FAMILY(NVNbuffer, Buffer);
    FAMILY(NVNtextureBuilder, TextureBuilder);
    FAMILY(NVNtextureView, TextureView);
    FAMILY(NVNtexture, Texture);
    FAMILY(NVNsamplerBuilder, SamplerBuilder);
    FAMILY(NVNsampler, Sampler);
    FAMILY(NVNtexturePool, TexturePool);
    FAMILY(NVNsamplerPool, SamplerPool);
    FAMILY(NVNprogram, Program);
    FAMILY(NVNwindowBuilder, WindowBuilder);
    FAMILY(NVNwindow, Window);
    FAMILY(NVNcommandBuffer, CommandBuffer);
    FAMILY(NVNblendState, BlendState);
    FAMILY(NVNcolorState, ColorState);
    FAMILY(NVNchannelMaskState, ChannelMaskState);
    FAMILY(NVNmultisampleState, MultisampleState);
    FAMILY(NVNpolygonState, PolygonState);
    FAMILY(NVNdepthStencilState, DepthStencilState);
    FAMILY(NVNvertexAttribState, VertexAttribState);
    FAMILY(NVNvertexStreamState, VertexStreamState);
    FAMILY(NVNdevice, Device);
    printf("%d calls checked\n", g_calls);
    return h_finish("t_obj_canary", &h);
}
