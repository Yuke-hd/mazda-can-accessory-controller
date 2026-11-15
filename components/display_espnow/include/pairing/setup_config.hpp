#pragma once
#include <cstring>
namespace pairing {
// Kept generic so the same setup policy is checked without ESP-IDF on the host.
template<class Config> Config setup_config(Config config, unsigned send_timeout_ticks) {
    config.sec_enable=true;
    config.forward_enable=false;
    std::memset(&config.receive_enable,0,sizeof(config.receive_enable));
    config.receive_enable.ack=true;
    config.receive_enable.group=true;
    config.send_max_timeout=send_timeout_ticks;
    return config;
}
}
