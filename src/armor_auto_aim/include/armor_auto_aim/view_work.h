/**
 * @project_name auto_aim
 * @file view.h
 * @brief
 * @author yx
 * @date 2023-12-24 17:24:23
 */

#ifndef ARMOR_AUTO_AIM_ARMOR_AUTO_AIM_VIEW_H
#define ARMOR_AUTO_AIM_ARMOR_AUTO_AIM_VIEW_H

#include <Eigen/Dense>
#include <QObject>

#include <armor_tracker/tracker.h>
#include <view/view.h>

namespace armor_auto_aim {
class ViewWork: public QObject {
    Q_OBJECT
public:
    explicit ViewWork(QObject* parent = nullptr);

    void show();
public slots:
    void showFrame(const cv::Mat& frame,
                   const std::vector<Armor>& armors, const Tracker& tracker,
                   const double& fps, const uint64_t& timestamp, const float& dt);

    void viewEkf(const armor_auto_aim::Tracker& tracker,
                 const Eigen::Vector3d& predict_camera_coordinate,
                 const Eigen::Vector3d& shoot_camera_coordinate) const;

    void viewEuler(const Eigen::Vector3d& imu, const Eigen::Vector3d& aim) const;

    void viewTimestamp(const uint64_t& camera_timestamp,
                       const uint64_t& imu_timestamp);

    void showThreadId() const { LOG(INFO) << "view_work_thread: " << QThread::currentThreadId(); }

    void viewFaceAngle(float yaw, float pre_yaw);

    void showFrameAimPoint(cv::Mat& src);
signals:
    void viewFaceAngleSign(float yaw, float pre_yaw);
private:
    view::View* m_view;
};
}

#endif //ARMOR_AUTO_AIM_ARMOR_AUTO_AIM_VIEW_H
