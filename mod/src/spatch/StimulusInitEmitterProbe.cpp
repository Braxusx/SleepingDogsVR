#include "StimulusInitEmitterProbe.h"

namespace spatch::stimulus {

const char* NameOf(InitEmitterState state) noexcept {
    switch (state) {
        case InitEmitterState::kNotSeen:
            return "NOT_SEEN";
        case InitEmitterState::kUnreadable:
            return "UNREADABLE";
        case InitEmitterState::kInvalid:
            return "INVALID";
        case InitEmitterState::kUninitialised:
            return "UNINITIALISED";
        case InitEmitterState::kValid:
            return "VALID";
    }
    return "UNREADABLE";
}

}  // namespace spatch::stimulus