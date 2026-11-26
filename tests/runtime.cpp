// The test process supplies the runtime implementation and retains its observations.
#include <openxr/openxr_loader_negotiation.h>

static PFN_xrGetInstanceProcAddr dispatch{};
extern "C" void setTestDispatch(PFN_xrGetInstanceProcAddr get) { dispatch = get; }
extern "C" XrResult xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo*,
                                                      XrNegotiateRuntimeRequest* req) {
    if (!dispatch)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->getInstanceProcAddr = dispatch;
    req->runtimeInterfaceVersion = 1;
    req->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    return XR_SUCCESS;
}
