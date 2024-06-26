/**
 * @project_name armor_auto_aiming
 * @file armor_auto_aim.cpp
 * @brief
 * @author yx
 * @date 2023-11-14 21:13:26
 */

#define USE_COS
//#define USE_SIN

#ifdef DEBUG
#include <QPointF>
#endif

#include <google_logger/google_logger.h>
#include <armor_auto_aim/armor_auto_aim.h>
#include <solver/coordinate_solver.h>
#include <solver/ballistic_solver.h>
#include <debug_toolkit/draw_package.h>

namespace armor_auto_aim {
ArmorAutoAim::ArmorAutoAim(const std::string& config_path, QObject* parent)
    : QThread(parent),
      m_config_path(config_path)
       {
#ifdef DEBUG
    qRegisterMetaType<Eigen::Vector3d>("Eigen::Vector3d");
    qRegisterMetaType<uint64_t>("uint64_t");
#endif
    loadConfig();
    m_hik_driver = std::make_unique<HikDriver>(m_params.hik_index);
    m_solver = Solver(m_solver_builder.build());
    m_detector = Detector(m_params.armor_model_path, m_solver.pnp_solver);
    m_detector.setDetectColor(m_params.target_color);
    initHikCamera();
    initEkf();
//    connect(m_hik_read_thread.getView(), &HikReadThread::readyData, this, &ArmorAutoAim::pushCameraData);
}

#ifdef DEBUG
void ArmorAutoAim::setViewWork(armor_auto_aim::ViewWork* vw) {
    m_view_work = vw;
}
#endif

void ArmorAutoAim::setSerialWork(armor_auto_aim::SerialWork* sw) {
    m_serial_work = sw;
}

void ArmorAutoAim::viewAimPoint(float x, float y, float z) {
    return;
    LOG(INFO) << "view aim point";
    auto quaternion = Eigen::Quaterniond(m_imu_data->quaternion.w, m_imu_data->quaternion.x,
                                                 m_imu_data->quaternion.y, m_imu_data->quaternion.z);
    auto p = m_solver.reproject(m_solver.worldToCamera(Eigen::Vector3d (x, y, z), quaternion.matrix()));
    LOG(INFO) << "p: " << p;
    cv::circle(m_frame, p, 26, cv::Scalar(255, 0, 0), -1);
//    emit showFrameAimPoint(m_frame);
}

void ArmorAutoAim::run() {
    using Clock = std::chrono::system_clock;
    using Unit = std::chrono::milliseconds;
//    LOG(INFO) << "auto_aim_thread: " << QThread::currentThreadId();
    cv::TickMeter tick_meter;
    float fps = 0.0f;
    int frame_count = 0;
    constexpr int from_fps_frame_count = 10;
    bool is_reset = true;
    uint64_t timestamp = ProgramClock::now().time_since_epoch().count();
    uint64_t last_t = timestamp;
    Eigen::Vector3d camera_point;
    Eigen::Quaterniond quaternion;

    if (!m_hik_driver->isConnected()) {
        unsigned int c = 0;
        int t = 2;
        while (!m_hik_driver->isConnected()) {
            LOG(WARNING) << fmt::format("try reconnect hik-camera...[{}]", c++);
            m_hik_driver->connectDriver(m_params.hik_index);
            std::this_thread::sleep_for(std::chrono::seconds(t));
            if (c > 10) t = 5;
        }
        initHikCamera();
    }

    while (true) {
        m_imu_data_queue.waitTop(*m_imu_data);
        m_aim_info.reset();
        auto ss = Clock::now();
//        LOG(INFO) << m_imu_data->to_string();
        // start fps clock
        if (is_reset) {
            tick_meter.reset();
            tick_meter.start();
            is_reset = false;
        }
        // -- main --
        // frame && timestamp
        m_hik_frame = m_hik_driver->getFrame();
        m_frame = m_hik_frame.getRgbFrame()->clone();
//        m_frame = *m_hik_frame.getRgbFrame();
        timestamp = m_hik_frame.getTimestamp();
        // dt
        dt = static_cast<float>(timestamp - last_t);
        LOG_IF(WARNING, dt > 100) << "dt: " << dt;
        last_t = timestamp;
        // -- detect --
//        auto s = Clock::now();
        m_detector.detect(m_frame, &m_armors);
//        auto e = Clock::now();
//        LOG(INFO) << fmt::format("detect latency: {} ms", std::chrono::duration_cast<Unit>(e - s).count());
        // getView world coordinate
        quaternion = Eigen::Quaterniond(m_imu_data->quaternion.w, m_imu_data->quaternion.x,
                                        m_imu_data->quaternion.y, m_imu_data->quaternion.z);
        for (auto& armor: m_armors) {
            camera_point = Eigen::Vector3d(armor.pose.x, armor.pose.y, armor.pose.z);
            armor.world_coordinate = m_solver.cameraToWorld(camera_point, quaternion.matrix());
//            armor.number = 6;
        }
        // -- tracker --
        if (m_tracker.state() == TrackerStateMachine::State::Lost) {
            m_tracker.initTracker(m_armors);
        } else {
            m_tracker.updateTracker(m_armors);
        }
        // -- predict --
        if (m_tracker.isTracking()) {
            const Eigen::VectorXd& predict_state = m_tracker.getTargetPredictSate();
            double xc  = predict_state[0],
                   yc  = predict_state[2],
                   zc  = predict_state[4],
                   yaw = predict_state[6],
                   r   = predict_state[8];
//            cv::circle(m_frame, m_solver.reproject(
//                    m_solver.worldToCamera(Eigen::Vector3d(xc, yc, zc), quaternion.matrix())),
//                       36, cv::Scalar(59, 188, 235), 8);
//            LOG(INFO) << "v_yaw: " << predict_state[7];
#ifdef USE_SIN
            Eigen::Vector3d world_predict_translation(xc - r*sin(yaw), yc - r*cos(yaw), zc);
#endif
#ifdef USE_COS
            Eigen::Vector3d world_predict_translation(xc - r*cos(yaw), yc - r*sin(yaw), zc);
#endif
            Eigen::Vector3d predict_translation = m_solver.worldToCamera(world_predict_translation, quaternion.matrix());
            cv::circle(m_frame, m_solver.reproject(predict_translation), 26, cv::Scalar(59, 10, 235), -1);
            double flight_time = std::abs(world_predict_translation.norm() / m_solver.getSpeed()) * 1000;
            double program_delay = static_cast<double>(
                    std::chrono::duration_cast<Unit>(
                            Clock::now().time_since_epoch()).count() - timestamp);
            double delay_time = m_params.delta_time + flight_time + program_delay;
            world_predict_translation[0] += predict_state[1] * delay_time;
            world_predict_translation[1] += predict_state[3] * delay_time;
            world_predict_translation[2] += predict_state[5] * delay_time;
            predict_translation = m_solver.worldToCamera(world_predict_translation, quaternion.matrix());
            cv::circle(m_frame, m_solver.reproject(predict_translation), 36, cv::Scalar(0, 0, 235), 8);
            m_aim_info = AutoAimInfo(
                    static_cast<float>(world_predict_translation[0]),
                    static_cast<float>(world_predict_translation[1]),
                    static_cast<float>(world_predict_translation[2]),
                    static_cast<float>(predict_state[1]),
                    static_cast<float>(predict_state[3]),
                    static_cast<float>(predict_state[5]),
                    static_cast<float>(predict_state[6]),
                    static_cast<float>(predict_state[7]),
                    static_cast<float>(r),
                    static_cast<float>(program_delay),
                    static_cast<uint8_t>(m_tracker.isTracking()));
//            cv::circle(m_frame, m_solver.reproject(
//                    m_solver.worldToCamera(Eigen::Vector3d(xc, yc, zc), quaternion.matrix())),
//                       40, cv::Scalar(0, 255, 255), -1);

            int armor_number = 1;
            double once_angle = /*M_PI_2*/ 2.0f * M_PI / 3.0f;  // 2.0f * M_PI / 3.0f
            for (int i = 0; i < armor_number; ++i) {
                double tmp_yaw = yaw + i * once_angle;
#ifdef USE_SIN
                Eigen::Vector3d p(xc - r*sin(tmp_yaw), yc - r*cos(tmp_yaw), zc);
#endif
#ifdef USE_COS
                Eigen::Vector3d p(xc - r*cos(tmp_yaw), yc - r*sin(tmp_yaw), zc);
#endif
                auto p2 = m_solver.reproject(m_solver.worldToCamera(p, quaternion.matrix()));
//                LOG(INFO) << "p2: " << p2;
                cv::circle(m_frame, p2, 20, cv::Scalar(0, 0, 255), -1);
                cv::putText(m_frame, std::to_string(i), p2+cv::Point2d(0, -30), 1, 2, cv::Scalar(0, 255, 55), 2);
                if (i == 0) {
                    float yaw = m_tracker.tracked_armor.pose.yaw * 180.0 / M_PI,
                          pitch = m_tracker.tracked_armor.pose.pitch * 180.0 / M_PI,
                          roll = m_tracker.tracked_armor.pose.roll * 180.0 / M_PI;
                    {
                        float x = m_tracker.tracked_armor.world_coordinate[0],
                              y = m_tracker.tracked_armor.world_coordinate[1],
                              z = m_tracker.tracked_armor.world_coordinate[2],
                              world_yaw = m_tracker.tracked_armor.pose.yaw;
//                        cv::putText(m_frame, std::to_string(world_yaw), p2+cv::Point2d(0, -250), 1, 2, cv::Scalar(0, 255, 55), 2);
//                        cv::putText(m_frame, std::to_string(world_yaw * 180.0 / M_PI), p2+cv::Point2d(0, -200), 1, 2, cv::Scalar(0, 255, 55), 2);
//                        cv::putText(m_frame, std::to_string(x), p2+cv::Point2d(0, -150), 1, 2, cv::Scalar(0, 255, 55), 2);
//                        cv::putText(m_frame, std::to_string(y), p2+cv::Point2d(0, -100), 1, 2, cv::Scalar(0, 255, 55), 2);
//                        cv::putText(m_frame, std::to_string(z), p2+cv::Point2d(0, -50), 1, 2, cv::Scalar(0, 255, 55), 2);
//
//                        cv::putText(m_frame, std::to_string(atan2(x, z) * 180.0 / M_PI), p2+cv::Point2d(250, -150), 1, 2, cv::Scalar(0, 255, 55), 2);
//                        cv::putText(m_frame, std::to_string(atan2(y, z) * 180.0 / M_PI), p2+cv::Point2d(250, -50), 1, 2, cv::Scalar(0, 255, 55), 2);
                    }
                }
//                break;
            }
#ifdef DEBUG
//            float yaw_ = m_tracker.tracked_armor.pose.yaw;
//            LOG(INFO) << "face yaw: " << yaw_ * 180 / M_PI;
//            emit m_view_work->viewFaceAngleSign(m_tracker.getLastYaw() * 180 / M_PI, predict_state[6] * 180 / M_PI);
//            emit viewEkfSign(m_tracker, predict_translation, predict_translation);
//            emit viewTimestampSign(timestamp, m_imu_data->timestamp);
#endif

/*            float xa = predict_translation[0],
                  ya = predict_translation[1],
                  za = predict_translation[2];
            double delta_pitch = m_solver.ballisticSolver(-Eigen::Vector3d(xa, ya, za));
            if (!std::isnan(delta_pitch)) {
                m_aim_info.pitch -= static_cast<float>(delta_pitch);
            } else {
                LOG(WARNING) << "delta_pitch is NaN";
            }
            m_aim_info.is_shoot = abs(m_aim_info.yaw) < 40.0f &&
                    abs(m_aim_info.pitch)  < 40.0f && abs(yaw * 180.0f / M_PI) < 30;
//            m_aim_info.is_shoot = 1;
            if (!((-40.0 < m_aim_info.yaw && m_aim_info.yaw < 40.0) &&
                  (-40.0 < m_aim_info.pitch && m_aim_info.pitch < 40.0))) {
                LOG(WARNING) << fmt::format("\narmors_size: {};\ntracked_armor-pose: {}",
                                            m_armors.size(),
                                            m_tracker.tracked_armor.pose.to_string())
                             << "\npredict: " << predict_translation << "; "
                             << "\ntarget_predict_state: " << m_tracker.getTargetPredictSate()
                             << "\ncommunicate info: " << m_aim_info.to_string();
                LOG(WARNING) << "异常值!";
            }*/
        } else {
            m_aim_info.reset();
            m_aim_info.tracker_status = static_cast<uint8_t>(m_tracker.isTracking());
        }
        // -- Send --
        auto imu_euler = quaternion.toRotationMatrix().eulerAngles(2, 1, 0);
#ifdef SENTRY
        m_aim_info.id = m_params.microcontroller_id;
#endif
#ifdef DEBUG
//        emit m_view_work->viewEuler(imu_euler, {m_aim_info.yaw * M_PI / 180, m_aim_info.pitch * M_PI / 180, 0});
#endif
//        LOG(INFO) << m_aim_info.to_string();
        emit sendAimInfo(m_aim_info);
        // -- debug --
        // - append information to frame(fps, tracker_info, timestamp, armors) -
        if (frame_count < from_fps_frame_count) {
            frame_count++;
        } else {
            tick_meter.stop();
            fps = static_cast<float>(++frame_count) / static_cast<float>(tick_meter.getTimeSec());
            frame_count = 0;
            is_reset = true;
        }
#ifdef DEBUG
        emit showFrame(m_frame, m_armors, m_tracker, fps, timestamp, dt);
#else
        LOG_EVERY_N(INFO, 10) << fmt::format("fps: {}", fps);
        LOG_EVERY_T(INFO, 10) << m_imu_data->to_string();
#endif
//        auto ee = Clock::now();
//        LOG(INFO) << fmt::format("latency: {} ms", std::chrono::duration_cast<Unit>(ee - ss).count());
    }
}

void ArmorAutoAim::loadConfig() {
    // detector
    m_config = YAML::LoadFile(m_config_path);
    if (!m_config)
        LOG(FATAL) << fmt::format("Failed: Invalid configuration file: {}!", m_config_path);
    const YAML::Node&& detector_config = m_config["detector"];
    m_params.hik_index = detector_config["camera"]["index"].as<int>();
    m_params.exp_time = detector_config["camera"]["exposure_time"].as<float>();
    m_params.gain = detector_config["camera"]["gain"].as<float>();
    m_params.armor_model_path = detector_config["armor_model_path"].as<std::string>();
    auto c = detector_config["detect_color"].as<std::string>();
    std::transform(c.begin(), c.end(), c.begin(), ::toupper);
    if (c == "RED")        m_params.target_color = ArmorColor::RED;
    else if (c == "BLUE")  m_params.target_color = ArmorColor::BLUE;
    else                   LOG(FATAL) << "Unknown color: " << c << "(transform to all upper case)";
    // solver config
    const YAML::Node&& solver_config = m_config["solver"];
    m_solver_builder.setG(solver_config["env"]["g"].as<double>());
    m_solver_builder.setBallSpeed(solver_config["ballistic"]["speed"].as<double>());
    m_solver_builder.setIntrinsicMatrix(solver_config["camera_params"]["intrinsic_matrix"].as<std::array<double, 9>>());
    m_solver_builder.setDistortionCess(solver_config["camera_params"]["distortion"].as<std::vector<double>>());
    auto T_ic_ci = solver_config["coordinate"]["T_ic"].as<std::vector<double>>();
    m_solver_builder.setTic(Eigen::Map<Eigen::Matrix4d>(T_ic_ci.data(), 4, 4).transpose());
    T_ic_ci = solver_config["coordinate"]["T_ci"].as<std::vector<double>>();
    m_solver_builder.setTci(Eigen::Map<Eigen::Matrix4d>(T_ic_ci.data(), 4, 4).transpose());
    auto tvec_i2w = solver_config["coordinate"]["tvec_i2w"].as<std::array<double, 3>>();
    Eigen::Vector3d tvec = Eigen::Map<Eigen::Vector3d>(tvec_i2w.data(), 3, 1);
    m_solver_builder.setTvecI2C(tvec);
    m_solver_builder.setTvecC2I(-tvec);
    // predict
    const YAML::Node&& predict_config = m_config["predict"];
    m_params.delta_time = predict_config["delta_time"].as<float>();
    auto q_dio = predict_config["kf"]["q"].as<std::array<double, 9>>();
    m_q_diagonal = Eigen::Map<Eigen::Matrix<double, 9, 1>>(q_dio.data(), 9, 1);
    auto r_dio = predict_config["kf"]["r"].as<std::array<double, 4>>();
    m_r_diagonal = Eigen::Map<Eigen::Vector4d>(r_dio.data(), 4, 1);
    // compensate 补偿
    const YAML::Node&& compensate_config = m_config["compensate"];
    m_params.compensate_yaw = compensate_config["yaw"].as<float>();
    m_params.compensate_pitch = compensate_config["pitch"].as<float>();

    m_params.microcontroller_id = m_config["microcontroller_id"].as<int>();
}

void ArmorAutoAim::initHikCamera() {
    if (m_hik_driver->isConnected()) {
        m_hik_driver->setExposureTime(m_params.exp_time);
        m_hik_driver->setGain(m_params.gain);
        m_hik_driver->showParamInfo();
        m_hik_driver->startReadThread();
#ifdef DEBUG
        m_hik_ui = std::make_unique<HikUi>(*m_hik_driver);
        m_hik_ui->show();
#endif
    }
}

void ArmorAutoAim::initEkf() {
    Q.diagonal() = m_q_diagonal;
    R.diagonal() = m_r_diagonal;
    int p = 100;
    Eigen::Matrix<double, 9, 9> p0;
    //  xa  vxa  ya  vya  za  vza  yaw v_yaw  r
    p0 << p,  0,   0,  0,  0,   0,   0,  0,   0, // xa
          0,  p,   0,  0,  0,   0,   0,  0,   0, // vxa
          0,  0,   p,  0,  0,   0,   0,  0,   0, // ya
          0,  0,   0,  p,  0,   0,   0,  0,   0, // vya
          0,  0,   0,  0,  p,   0,   0,  0,   0, // za
          0,  0,   0,  0,  0,   p,   0,  0,   0, // vza
          0,  0,   0,  0,  0,   0,   p,  0,   0, // yaw
          0,  0,   0,  0,  0,   0,   0,  p,   0, // v_yaw
          0,  0,   0,  0,  0,   0,   0,  0,   p; // r
    auto f = [this](const Eigen::MatrixXd& x)->Eigen::MatrixXd {
//        LOG(INFO) << "\nx: " << x;
        Eigen::VectorXd x_new = x;
        x_new(0) += x(1) * dt;
        x_new(2) += x(3) * dt;
        x_new(4) += x(5) * dt;
        x_new(6) += x(7) * dt;
        return x_new;
    };
    auto j_f = [this](const Eigen::MatrixXd&)->Eigen::MatrixXd {
        Eigen::Matrix<double, 9, 9> F;
        //   x  vx  y  vy   z   yz  yaw v_yaw  r
        F << 1, dt, 0,  0,  0,  0,   0,  0,    0, // x
             0, 1,  0,  0,  0,  0,   0,  0,    0, // vx
             0, 0,  1,  dt, 0,  0,   0,  0,    0, // y
             0, 0,  0,  1,  0,  0,   0,  0,    0, // vy
             0, 0,  0,  0,  1,  dt,  0,  0,    0, // z
             0, 0,  0,  0,  0,  1,   0,  0,    0, // vz
             0, 0,  0,  0,  0,  0,   1,  dt,   0, // yaw
             0, 0,  0,  0,  0,  0,   0,  1,    0, // v_yaw
             0, 0,  0,  0,  0,  0,   0,  0,    1; // r
        return F;
    };

    auto h = [](const Eigen::VectorXd& x)->Eigen::MatrixXd {
        Eigen::VectorXd z(4);
        double xc = x[0], yc = x[2], yaw = x[6], r = x[8];
#ifdef USE_SIN
        z[0] = xc - r*sin(yaw);
        z[1] = yc - r*cos(yaw);
#endif
#ifdef USE_COS
        z[0] = xc - r*cos(yaw);
        z[1] = yc - r*sin(yaw);
#endif
        z[2] = x[4];
        z[3] = x[6];
        return z;
    };
    auto j_h = [](const Eigen::VectorXd& x)->Eigen::MatrixXd {
        Eigen::MatrixXd h(4, 9);
        double yaw = x[6], r = x[8];
#ifdef USE_SIN
        //   x  vx  y  vy   z yz     yaw       v_yaw     r
        h << 1, 0,  0,  0,  0, 0,  -r*cos(yaw),   0,   -sin(yaw), // x
             0, 0,  1,  0,  0, 0,   r*sin(yaw),   0,   -cos(yaw), // y
             0, 0,  0,  0,  1, 0,     0      ,   0,      0,       // z
             0, 0,  0,  0,  0, 0,     1      ,   0,      0;       // yaw
#endif
#ifdef USE_COS
        //   x  vx  y  vy   z yz     yaw       v_yaw     r
        h << 1, 0,  0,  0,  0, 0,  r*sin(yaw),   0,   -cos(yaw), // x
             0, 0,  1,  0,  0, 0, -r*cos(yaw),   0,   -sin(yaw), // y
             0, 0,  0,  0,  1, 0,     0      ,   0,      0,      // z
             0, 0,  0,  0,  0, 0,     1      ,   0,      0;      // yaw
#endif
        return h;
    };
    auto update_Q = [this]()->Eigen::MatrixXd {
        if (m_tracker.ekf->getQ().size() == 0) {
            return Q;
        } else {
            return m_tracker.ekf->getQ();
        }
        double s2qxyz = 20.0,
               s2qyaw = 100.0,
               s2qr   = 800.0;
        Eigen::MatrixXd q(9, 9);
        double t       = dt,
               x       = s2qxyz,
               yaw     = s2qyaw,
               r       = s2qr,
               q_x_x   = pow(t, 4) / 4 * x,
               q_x_vx  = pow(t, 3) / 2 * x,
               q_vx_vx = pow(t, 2) * x,
               q_y_y   = pow(t, 4) / 4 * yaw,
               q_y_vy  = pow(t, 3) / 2 * x,
               q_vy_vy = pow(t, 2) * yaw,
               q_r     = pow(t, 4) / 4 * r;
        //    xc      v_xc    yc      v_yc    za      v_za    yaw     v_yaw   r
        q << q_x_x,  q_x_vx, 0,      0,      0,      0,      0,      0,      0,
             q_x_vx, q_vx_vx,0,      0,      0,      0,      0,      0,      0,
             0,      0,      q_x_x,  q_x_vx, 0,      0,      0,      0,      0,
             0,      0,      q_x_vx, q_vx_vx,0,      0,      0,      0,      0,
             0,      0,      0,      0,      q_x_x,  q_x_vx, 0,      0,      0,
             0,      0,      0,      0,      q_x_vx, q_vx_vx,0,      0,      0,
             0,      0,      0,      0,      0,      0,      q_y_y,  q_y_vy, 0,
             0,      0,      0,      0,      0,      0,      q_y_vy, q_vy_vy,0,
             0,      0,      0,      0,      0,      0,      0,      0,      q_r;
        return q;
    };
    auto update_R = [this](const Eigen::MatrixXd&)->Eigen::MatrixXd {
        if (m_tracker.ekf->getR().size() == 0) {
            return R;
        } else {
            return m_tracker.ekf->getR();
        }
    };

    m_tracker.ekf = std::make_shared<ExtendedKalmanFilter>(p0, f, h, j_f, j_h, update_Q, update_R);
}

//AutoAimInfo ArmorAutoAim::translation2YawPitch(const solver::Pose& pose) {
//    return {
//        static_cast<float>(atan2(pose.x, pose.z) * 180.0f / M_PI),
//        static_cast<float>(atan2(pose.y, pose.z) * 180.0f / M_PI),
//        pose.z
//    };
//}
//
//AutoAimInfo ArmorAutoAim::translation2YawPitch(const Eigen::Vector3d& translation) {
//    return {
//        static_cast<float>(atan2(translation(0), translation(2)) * 180.0f / M_PI),
//        static_cast<float>(atan2(translation(1), translation(2)) * 180.0f / M_PI),
//        static_cast<float>(translation(2))
//    };
//}
}
