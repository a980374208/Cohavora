#include <cstdlib>
// The packaged SDK omits this no-message checks overload. Keep fatal checks
// fatal in the isolated backend; never print potentially sensitive context.
namespace webrtc::webrtc_checks_impl {
void FatalLog(const char*, int) { std::abort(); }
}
