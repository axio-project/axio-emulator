#ifndef QPINFO_H
#define QPINFO_H

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

const size_t MAX_HOSTNAME_LEN = 64;
const size_t MAX_NIC_NAME_LEN = 64;

class QPInfo {
   public:
    uint32_t qp_num;                  // QP编号
    uint16_t lid;                     // 本地标识符 (LID)
    uint8_t gid[16];                  // 全球标识符 (GID)
    uint32_t mtu;                     // 最大传输单元 (MTU)
    char hostname[MAX_HOSTNAME_LEN];  // 主机名
    char nic_name[MAX_NIC_NAME_LEN];  // 网络接口名 (如 rdma0)

    // 构造函数
    QPInfo(uint32_t qp_num = 0, uint16_t lid = 0,
           const uint8_t* gid_ptr = nullptr, uint32_t mtu = 0,
           const std::string& hostname = "", const std::string& nic_name = "")
        : qp_num(qp_num), lid(lid), mtu(mtu) {
        if (gid_ptr != nullptr) {
            std::memcpy(gid, gid_ptr, 16);
        } else {
            std::memset(gid, 0, 16);  // 如果未指定GID，则初始化为0
        }
        std::strncpy(this->hostname, hostname.c_str(), MAX_HOSTNAME_LEN);
        this->hostname[MAX_HOSTNAME_LEN - 1] = '\0';  // 确保字符串以 '\0' 结尾

        std::strncpy(this->nic_name, nic_name.c_str(), MAX_NIC_NAME_LEN);
        this->nic_name[MAX_NIC_NAME_LEN - 1] = '\0';  // 确保字符串以 '\0' 结尾
    }

    // 拷贝构造函数
    QPInfo(const QPInfo& other) {
        qp_num = other.qp_num;
        lid = other.lid;
        mtu = other.mtu;
        std::memcpy(gid, other.gid, 16);
        std::strncpy(hostname, other.hostname, MAX_HOSTNAME_LEN);
        std::strncpy(nic_name, other.nic_name, MAX_NIC_NAME_LEN);
    }

    QPInfo& operator=(const QPInfo& other) {
        if (this != &other) {
            qp_num = other.qp_num;
            lid = other.lid;
            mtu = other.mtu;
            std::memcpy(gid, other.gid, 16);
            std::strncpy(hostname, other.hostname, MAX_HOSTNAME_LEN);
            std::memcpy(nic_name, other.nic_name, MAX_NIC_NAME_LEN);
        }
        return *this;
    }

    // 输出信息
    void print() const {
        std::cout << "QP Number: " << qp_num << "\n";
        std::cout << "LID: " << lid << "\n";
        std::cout << "GID: ";
        for (int i = 0; i < 16; ++i) {
            std::cout << std::hex << static_cast<int>(gid[i]);
            if (i < 15) std::cout << ":";
        }
        std::cout << "\n";
        std::cout << "MTU: " << mtu << "\n";
        std::cout << "Hostname: " << hostname << "\n";
        std::cout << "NIC Name: " << nic_name << "\n";
    }

    // 设置GID
    void set_gid(const uint8_t* gid_ptr) {
        if (gid_ptr != nullptr) {
            std::memcpy(gid, gid_ptr, 16);
        }
    }

    friend std::ostream& operator<<(std::ostream& os, const QPInfo& info) {
        os << "Hostname: " << info.hostname << ", "
           << "NIC Name: " << info.nic_name << ", MTU: " << info.mtu << ", "
           << "QP Number: " << info.qp_num << ", "
           << "LID: " << info.lid << ", "
           << "GID: ";
        for (int i = 0; i < 16; ++i) {
            os << std::hex << static_cast<int>(info.gid[i]);
            if (i < 15) os << ":";
        }

        return os;  // 返回 os 以兼容 ostream
    }

    std::string to_string() const {
        auto ss = std::stringstream();
        ss << *this;
        return ss.str();
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> data;
        data.resize(sizeof(qp_num) + sizeof(lid) + sizeof(gid) + sizeof(mtu) +
                    MAX_HOSTNAME_LEN + MAX_NIC_NAME_LEN);

        uint8_t* ptr = data.data();

        std::memcpy(ptr, &qp_num, sizeof(qp_num));
        ptr += sizeof(qp_num);

        std::memcpy(ptr, &lid, sizeof(lid));
        ptr += sizeof(lid);

        std::memcpy(ptr, gid, sizeof(gid));
        ptr += sizeof(gid);

        std::memcpy(ptr, &mtu, sizeof(mtu));
        ptr += sizeof(mtu);

        std::memcpy(ptr, hostname, MAX_HOSTNAME_LEN);
        ptr += MAX_HOSTNAME_LEN;

        std::memcpy(ptr, nic_name, MAX_NIC_NAME_LEN);

        return data;
    }

    // 反序列化方法
    static QPInfo deserialize(const std::vector<uint8_t>& data) {
        QPInfo qp_info;
        const uint8_t* ptr = data.data();

        std::memcpy(&qp_info.qp_num, ptr, sizeof(qp_info.qp_num));
        ptr += sizeof(qp_info.qp_num);

        std::memcpy(&qp_info.lid, ptr, sizeof(qp_info.lid));
        ptr += sizeof(qp_info.lid);

        std::memcpy(qp_info.gid, ptr, sizeof(qp_info.gid));
        ptr += sizeof(qp_info.gid);

        std::memcpy(&qp_info.mtu, ptr, sizeof(qp_info.mtu));
        ptr += sizeof(qp_info.mtu);

        std::memcpy(qp_info.hostname, ptr, MAX_HOSTNAME_LEN);
        ptr += MAX_HOSTNAME_LEN;

        std::memcpy(qp_info.nic_name, ptr, MAX_NIC_NAME_LEN);

        return qp_info;
    }
};

// void collect_global_qp_info(
//     const std::vector<QPInfo>& local_qp_info,
//     std::map<std::string, std::map<std::string, QPInfo>>& all_qp_infos,
//     bool print = false);

#endif  // QPINFO_H
