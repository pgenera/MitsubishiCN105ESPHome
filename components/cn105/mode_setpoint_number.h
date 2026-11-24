#pragma once

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include <functional>

namespace esphome {

class ModeSetpointNumber : public number::Number, public Component {
 public:
    using CallbackFunction = std::function<void(float value)>;

    void setCallbackFunction(CallbackFunction&& callback) {
        this->callback_ = std::move(callback);
    }

 protected:
    void control(float value) override {
        if (this->callback_) {
            this->callback_(value);
        }
    }

 private:
    CallbackFunction callback_;
};

}  // namespace esphome
