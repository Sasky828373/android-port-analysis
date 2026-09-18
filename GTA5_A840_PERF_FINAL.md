# GTA V Android A840 performance plan

This branch keeps the known-working old31 DXVK Native renderer and targets lower overhead without reducing image quality.

## Production DXVK config

```ini
dxgi.syncInterval = 0
dxgi.maxFrameRate = 120
dxgi.maxFrameLatency = 1
dxvk.numCompilerThreads = 4
d3d11.cachedDynamicResources = vi
d3d11.relaxedBarriers = True
```

Do not force unsupported descriptor-buffer/descriptor-heap paths.
Do not disable Vulkan synchronization, LRZ, UBWC, or force GMEM/SYSMEM.
Do not replace old31 until the Android SDL2/WSI patches are reproduced.

## Next source-level targets

1. Keep shader/pipeline cache enabled and persistent.
2. Prewarm D3D11 shaders/pipelines during loading where the port permits it.
3. Batch constant-buffer writes and prefer sub-range binding instead of many tiny updates.
4. Avoid redundant resource state changes and partial UpdateSubresource writes between draws.
5. Keep logging/telemetry disabled in production.
6. Investigate GTA's grVulkanRuntime/grVulkanRenderGraph as a future native-Vulkan backend, but do not remove DXVK until that backend is proven complete.

The native-Vulkan path is experimental and must be validated for device/swapchain/presentation, shader translation, resource lifetime, and full render coverage before replacing D3D11/DXVK.
