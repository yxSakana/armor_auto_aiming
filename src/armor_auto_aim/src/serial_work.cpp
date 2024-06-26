/**
 * @project_name auto_aim
 * @file serial.cpp
 * @brief
 * @author yx
 * @date 2023-12-24 18:17:18
 */

#include <QTimer>

#include <armor_auto_aim/serail_work.h>

namespace armor_auto_aim {
SerialWork::SerialWork(QObject* parent)
        : QObject(parent) {
    qRegisterMetaType<uint8_t>("uint8_t");
    qRegisterMetaType<uint16_t>("uint16_t");
    qRegisterMetaType<ImuData>("ImuData");
    qRegisterMetaType<AutoAimInfo>("AutoAimInfo");

    connect(&m_serial, &VCOMCOMM::receiveData, this, &SerialWork::selectFunction);
}

void SerialWork::sendAimInfo(const AutoAimInfo& aim_info) {
    QByteArray data;
    data.resize(sizeof(aim_info));
    std::memcpy(data.data(), &aim_info, sizeof(aim_info));
    emit m_serial.CrossThreadTransmitSignal(m_SendAutoAimInfoCode, m_PcId, data);
}

void SerialWork::sendNowTimestamp() {
    uint64_t timestamp = std::chrono::duration_cast<ClockUnit>(ProgramClock ::now().time_since_epoch()).count();
    QByteArray data;
    data.resize(sizeof(timestamp));
    std::memcpy(data.data(), &timestamp, sizeof(timestamp));
    emit m_serial.CrossThreadTransmitSignal(m_SendTimestampCode, m_PcId, data);
}

void SerialWork::selectFunction(uint8_t code, uint16_t id, const QByteArray& data) {
    if (id == m_MicrocontrollerId) {
//        LOG(INFO) << fmt::format("code: {}; id: {}; data: {}", code, id, data.data());
        switch (code) {
            case m_RecvImuInfoCode: {
                ImuData imu_data;
                std::memcpy(&imu_data, data.data(), sizeof(imu_data));
                emit readyImuData(imu_data);
                break;
            }
            case m_RecvTimestampCode: {
                sendNowTimestamp();
                break;
            }
            case m_RecvAimPointCode: {
                LOG(INFO) << "get point";
                AimPoint point;
                std::memcpy(&point, data.data(), sizeof(AimPoint));
                emit aimPoint(point.x, point.y, point.z);
                break;
            }
            default: {
                LOG(WARNING) << fmt::format("Unknown function code {}, id: {}, data: {}", code, id, data.data());
            }
        }
    } else {
        LOG(WARNING) << "Unknown id: " << id;
    }
}
}
