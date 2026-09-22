# GTA V native Vulkan compatibility renderer
Source-built layer for migrating the remaining RageDirect3DDeviceContext11 path to Vulkan.

Phase 1:
- attaches to existing Vulkan instance/device/queue
- transient command pool
- reusable descriptor pool
- stable C ABI
- no graphics-quality changes

Migration order before DXVK removal:
1. vertex/index buffers + input layouts
2. VS/PS/CS shaders + constant buffers
3. SRV/UAV/samplers
4. render/depth targets + viewport/scissor
5. Draw/DrawIndexed/Dispatch
6. Copy/Clear/Resolve
7. swapchain/present

DXVK remains until all paths are redirected and validated.
