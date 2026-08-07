#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace axio {

inline constexpr size_t kMaxHostnameLength = 64;
inline constexpr size_t kMaxNicNameLength = 64;

/** Passive queue-pair handshake record with stable field and wire ordering. */
struct QueuePairInfo {
  uint32_t queue_pair_number_;
  uint16_t lid_;
  uint8_t gid_[16];
  uint8_t gid_table_index_;
  uint32_t mtu_;
  uint8_t mac_address_[6];
  char hostname_[kMaxHostnameLength];
  char nic_name_[kMaxNicNameLength];
  bool initialized_;

  QueuePairInfo(uint32_t queue_pair_number = 0, uint16_t lid = 0,
                const uint8_t* gid = nullptr, uint32_t mtu = 0,
                const std::string& hostname = "",
                const std::string& nic_name = "")
      : queue_pair_number_(queue_pair_number),
        lid_(lid),
        gid_table_index_(0),
        mtu_(mtu),
        initialized_(false) {
    if (gid != nullptr) {
      std::memcpy(this->gid_, gid, sizeof(this->gid_));
    } else {
      std::memset(this->gid_, 0, sizeof(this->gid_));
    }
    std::memset(this->mac_address_, 0, sizeof(this->mac_address_));
    std::strncpy(this->hostname_, hostname.c_str(), kMaxHostnameLength);
    this->hostname_[kMaxHostnameLength - 1] = '\0';
    std::strncpy(this->nic_name_, nic_name.c_str(), kMaxNicNameLength);
    this->nic_name_[kMaxNicNameLength - 1] = '\0';
  }

  QueuePairInfo(const QueuePairInfo& other) {
    this->queue_pair_number_ = other.queue_pair_number_;
    this->lid_ = other.lid_;
    this->mtu_ = other.mtu_;
    this->gid_table_index_ = other.gid_table_index_;
    std::memcpy(this->gid_, other.gid_, sizeof(this->gid_));
    std::memcpy(this->mac_address_, other.mac_address_,
                sizeof(this->mac_address_));
    std::strncpy(this->hostname_, other.hostname_, kMaxHostnameLength);
    std::strncpy(this->nic_name_, other.nic_name_, kMaxNicNameLength);
    this->initialized_ = other.initialized_;
  }

  QueuePairInfo& operator=(const QueuePairInfo& other) {
    if (this != &other) {
      this->queue_pair_number_ = other.queue_pair_number_;
      this->lid_ = other.lid_;
      this->mtu_ = other.mtu_;
      this->gid_table_index_ = other.gid_table_index_;
      std::memcpy(this->gid_, other.gid_, sizeof(this->gid_));
      std::memcpy(this->mac_address_, other.mac_address_,
                  sizeof(this->mac_address_));
      std::strncpy(this->hostname_, other.hostname_, kMaxHostnameLength);
      std::strncpy(this->nic_name_, other.nic_name_, kMaxNicNameLength);
      this->initialized_ = other.initialized_;
    }
    return *this;
  }

  void print() const {
    std::cout << "QP Number: " << std::dec << this->queue_pair_number_ << "\n";
    std::cout << "LID: " << std::dec << this->lid_ << "\n";
    std::cout << "MTU: " << std::dec << this->mtu_ << "\n";
    std::cout << "GID Table Index: " << std::dec
              << static_cast<int>(this->gid_table_index_) << "\n";
    std::cout << "GID: ";
    for (int i = 0; i < 16; ++i) {
      std::cout << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(this->gid_[i]);
      if (i < 15) {
        std::cout << ":";
      }
    }
    std::cout << std::dec << "\n";
    std::cout << "MAC Address: ";
    for (int i = 0; i < 6; ++i) {
      std::cout << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(this->mac_address_[i]);
      if (i < 5) {
        std::cout << ":";
      }
    }
    std::cout << std::dec << "\n";
    std::cout << "Hostname: " << this->hostname_ << "\n";
    std::cout << "NIC Name: " << this->nic_name_ << "\n";
    std::cout << "Initialized: " << (this->initialized_ ? "Yes" : "No")
              << "\n";
  }

  void set_gid(const uint8_t* gid) {
    if (gid != nullptr) {
      std::memcpy(this->gid_, gid, sizeof(this->gid_));
    }
  }

  void set_mac(const uint8_t* mac_address) {
    if (mac_address != nullptr) {
      std::memcpy(this->mac_address_, mac_address,
                  sizeof(this->mac_address_));
    }
  }

  friend std::ostream& operator<<(std::ostream& output,
                                  const QueuePairInfo& info) {
    output << "Hostname: " << info.hostname_ << ", "
           << "NIC Name: " << info.nic_name_ << ", MTU: " << info.mtu_ << ", "
           << "QP Number: " << info.queue_pair_number_ << ", "
           << "LID: " << info.lid_ << ", "
           << "GID Index: " << static_cast<int>(info.gid_table_index_) << ", "
           << "MAC: ";
    for (int i = 0; i < 6; ++i) {
      output << std::hex << static_cast<int>(info.mac_address_[i]);
      if (i < 5) {
        output << ":";
      }
    }
    output << ", GID: ";
    for (int i = 0; i < 16; ++i) {
      output << std::hex << static_cast<int>(info.gid_[i]);
      if (i < 15) {
        output << ":";
      }
    }
    return output;
  }

  std::string to_string() const {
    std::stringstream stream;
    stream << *this;
    return stream.str();
  }

  std::string serialize() const {
    std::string serialized_data;
    serialized_data +=
        "qp_num:" + std::to_string(this->queue_pair_number_) + ";";
    serialized_data += "lid:" + std::to_string(this->lid_) + ";";
    serialized_data += "gid:";
    for (int i = 0; i < 16; i++) {
      serialized_data += std::to_string(static_cast<int>(this->gid_[i])) + ",";
    }
    serialized_data += ";gid_table_index:" +
                       std::to_string(
                           static_cast<int>(this->gid_table_index_)) +
                       ";";
    serialized_data += "mac:";
    for (int i = 0; i < 6; i++) {
      serialized_data +=
          std::to_string(static_cast<int>(this->mac_address_[i])) + ",";
    }
    serialized_data += ";mtu:" + std::to_string(this->mtu_) + ";";
    serialized_data += "hostname:" + std::string(this->hostname_) + ";";
    serialized_data += "nic_name:" + std::string(this->nic_name_) + ";";
    serialized_data +=
        "is_initialized:" + std::to_string(this->initialized_);
    return serialized_data;
  }

  void deserialize(const std::string& serialized_data) {
    std::istringstream input(serialized_data);
    std::string token;

    while (std::getline(input, token, ';')) {
      std::istringstream token_stream(token);
      std::string key;
      std::string value;
      std::getline(token_stream, key, ':');
      std::getline(token_stream, value, ':');

      if (key == "qp_num") {
        this->queue_pair_number_ = static_cast<uint32_t>(std::stoi(value));
      } else if (key == "lid") {
        this->lid_ = static_cast<uint16_t>(std::stoi(value));
      } else if (key == "gid") {
        std::istringstream gid_stream(value);
        std::string gid_token;
        int i = 0;
        while (std::getline(gid_stream, gid_token, ',') && i < 16) {
          this->gid_[i++] = static_cast<uint8_t>(std::stoi(gid_token));
        }
      } else if (key == "gid_table_index") {
        this->gid_table_index_ = static_cast<uint8_t>(std::stoi(value));
      } else if (key == "mac") {
        std::istringstream mac_stream(value);
        std::string mac_token;
        int i = 0;
        while (std::getline(mac_stream, mac_token, ',') && i < 6) {
          this->mac_address_[i++] =
              static_cast<uint8_t>(std::stoi(mac_token));
        }
      } else if (key == "mtu") {
        this->mtu_ = static_cast<uint32_t>(std::stoi(value));
      } else if (key == "hostname") {
        std::strncpy(this->hostname_, value.c_str(),
                     sizeof(this->hostname_) - 1);
        this->hostname_[sizeof(this->hostname_) - 1] = '\0';
      } else if (key == "nic_name") {
        std::strncpy(this->nic_name_, value.c_str(),
                     sizeof(this->nic_name_) - 1);
        this->nic_name_[sizeof(this->nic_name_) - 1] = '\0';
      } else if (key == "is_initialized") {
        this->initialized_ = std::stoi(value) != 0;
      }
    }
  }
};

}  // namespace axio
