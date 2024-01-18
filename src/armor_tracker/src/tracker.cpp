/**
 * @projectName armor_auto_aim
 * @file track.cpp
 * @brief 
 * 
 * @author yx 
 * @date 2023-11-03 19:29
 */

#include <armor_tracker/tracker.h>

#include <glog/logging.h>

namespace armor_auto_aim {
void TrackerStateMachine::update(bool detector_result) {
    if (m_state == State::Lost) {
        if (!detector_result) {
            m_detect_count = 0;
            m_lost_count = 0;
        }
    } else if (m_state == State::Detecting) {
        if (detector_result) {
            if (++m_detect_count > m_tracking_threshold) m_state = State::Tracking;
        } else {
            m_detect_count = 0;
            m_state = State::Lost;
        }
    } else if (m_state == State::Tracking) {
        if (!detector_result) m_state = State::TempLost;

    } else if (m_state == State::TempLost) {
        if (detector_result) {
            m_lost_count = 0;
            m_state = State::Tracking;
        } else {
            if (++m_lost_count > m_lost_threshold) {
                m_lost_count = 0;
                m_state = State::Lost;
            }
        }
    }
}

void Tracker::initTracker(const Armors& armors) {
    LOG_IF(ERROR, ekf == nullptr) << "ekf is nullptr";
    if (armors.empty() || ekf == nullptr)
        return;
    // 选择要跟踪的装甲板(优先选择距离最近的)(en: Select tracked armor)
    double min_distance = DBL_MAX;
    tracked_armor = armors[0];
    for (const auto& armor: armors) {
        // TODO: 是否需要改为 (z, x) 与 image_point(x, y)的距离
        if (armor.world_coordinate[2] < min_distance) {
            min_distance = armor.world_coordinate[2];
            tracked_armor = armor;
        }
    }
    // initialization
    initEkf(tracked_armor);
    m_tracker_state_machine.initState();
    m_tracked_id = tracked_armor.number;
}

void Tracker::updateTracker(const Armors& armors) {
    // prior
    bool is_matched = false;
    const Armor* same_id_armor;
    int same_id_armor_count = 0;
    m_target_predict_state = ekf->update();
    // 寻找tracked装甲板
    if (!armors.empty()) {
//        m_target_predict_state = ekf->update();
        double min_position_difference = DBL_MAX;
        double yaw_difference = DBL_MAX;
        Eigen::Vector3d measurement_position_vec{};
        Eigen::Vector3d predicted_position_vec(m_target_predict_state(0),
                                               m_target_predict_state(2),
                                               m_target_predict_state(4));
        for (const auto& armor: armors) {
            if (armor.number == m_tracked_id) {
               same_id_armor = &armor;
               same_id_armor_count++;
            }
            measurement_position_vec = armor.world_coordinate;
            double position_difference = (predicted_position_vec - measurement_position_vec).norm();
            if (position_difference < min_position_difference) {
                min_position_difference = position_difference;
                yaw_difference = std::abs(m_target_predict_state[6] - armor.pose.yaw);
                tracked_armor = armor;
            }
        }
        // 后验及装甲板跳变处理
        if (min_position_difference < m_MaxMatchDistance &&
            yaw_difference < m_MaxMatchYaw) {
//             TODO: 是否需要使用shortestAngularDistance 对 yaw 进行处理
            is_matched = true;
            measurement = Eigen::Vector4d(tracked_armor.world_coordinate[0], tracked_armor.world_coordinate[1],
                                          tracked_armor.world_coordinate[2], tracked_armor.pose.yaw);
            m_target_predict_state = ekf->predict(measurement);
        } else if (same_id_armor_count == 1) {
            LOG_IF(WARNING, min_position_difference > m_MaxMatchDistance)
                    << min_position_difference
                    << "\n predicted_position_vec: " << predicted_position_vec
                    << "\n measurement_position_vec: " << measurement_position_vec;;
            LOG(WARNING) << "armor jump";
            handleArmorJump(*same_id_armor);
        } else {
            LOG_IF(WARNING, min_position_difference > m_MaxMatchDistance)
                    << min_position_difference
                    << "\n predicted_position_vec: " << predicted_position_vec
                    << "\n measurement_position_vec: " << measurement_position_vec
                    << "\n size: " << armors.size()
                    << "\n armor: " << armors[0].world_coordinate
                    << "\n id: " << m_tracked_id << " ?= " << armors[0].number;                  
            LOG(WARNING) << "No matched armor!";
        }
    }
    // update
    m_tracker_state_machine.update(is_matched);
}

void Tracker::initEkf(const Armor& armor) {
    double xa = armor.world_coordinate[0];
    double ya = armor.world_coordinate[1];
    double za = armor.world_coordinate[2];
    double yaw = armor.pose.yaw;

    m_target_predict_state = Eigen::VectorXd::Zero(8);
    m_target_predict_state << xa, 0, ya, 0, za, 0, yaw, 0;

    Eigen::Matrix<double, 8, 8> p0;
    double p = 10000;
    //  xa  vxa  ya  vya  za  vza  yaw v_yaw
    p0 << p,  0,   0,  0,  0,   0,   0,  0, // xa
          0,  p,   0,  0,  0,   0,   0,  0, // vxa
          0,  0,   p,  0,  0,   0,   0,  0, // ya
          0,  0,   0,  p,  0,   0,   0,  0, // vya
          0,  0,   0,  0,  p,   0,   0,  0, // za
          0,  0,   0,  0,  0,   p,   0,  0, // vza
          0,  0,   0,  0,  0,   0,   p,  0, // yaw
          0,  0,   0,  0,  0,   0,   0,  p; // v_yaw
    ekf->initEkf(m_target_predict_state, p0);
}

void Tracker::handleArmorJump(const armor_auto_aim::Armor& same_id_armor) {
    m_target_predict_state[0] = same_id_armor.world_coordinate[0];
    m_target_predict_state[1] = 0;
    m_target_predict_state[2] = same_id_armor.world_coordinate[1];
    m_target_predict_state[3] = 0;
    m_target_predict_state[4] = same_id_armor.world_coordinate[2];
    m_target_predict_state[5] = 0;
    m_target_predict_state[6] = same_id_armor.pose.yaw;
    m_target_predict_state[7] = 0;
}
} // armor_auto_aim