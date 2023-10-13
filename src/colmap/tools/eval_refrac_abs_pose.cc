#include "colmap/estimators/generalized_pose.h"
#include "colmap/geometry/rigid3.h"
#include "colmap/math/math.h"
#include "colmap/math/random.h"
#include "colmap/scene/camera.h"

#include <fstream>

using namespace colmap;

struct PointsData {
  std::vector<Eigen::Vector2d> points2D;
  std::vector<Eigen::Vector2d> points2D_refrac;
  std::vector<Eigen::Vector3d> points3D;

  std::vector<Camera> virtual_cameras;
  std::vector<Rigid3d> virtual_from_reals;

  Rigid3d cam_from_world_gt;
};

void GenerateRandom3D2DPoints(const Camera& camera,
                              size_t num_points,
                              const Rigid3d& cam_from_world_gt,
                              PointsData& points_data,
                              double noise_level,
                              double inlier_ratio) {
  // Generate refractive image points first, because flatport reduces FOV.

  points_data.points2D.clear();
  points_data.points2D_refrac.clear();
  points_data.points3D.clear();

  points_data.points2D.reserve(num_points);
  points_data.points2D_refrac.reserve(num_points);
  points_data.points3D.reserve(num_points);
  points_data.cam_from_world_gt = cam_from_world_gt;

  size_t num_inliers =
      static_cast<size_t>(static_cast<double>(num_points) * inlier_ratio);
  size_t cnt = 0;
  for (size_t i = 0; i < num_points; i++) {
    Eigen::Vector2d point2D_refrac;
    point2D_refrac.x() =
        RandomUniformReal(0.5, static_cast<double>(camera.Width()) - 0.5);
    point2D_refrac.y() =
        RandomUniformReal(0.5, static_cast<double>(camera.Height()) - 0.5);

    const double depth = RandomUniformReal(0.5, 10.0);
    const Eigen::Vector3d point3D_local =
        camera.CamFromImgRefracPoint(point2D_refrac, depth);

    const Eigen::Vector3d point3D_world =
        Inverse(cam_from_world_gt) * point3D_local;

    // Now project the point in-air.
    Eigen::Vector2d point2D = camera.ImgFromCam(point3D_local.hnormalized());

    if (cnt < num_inliers) {
      // Add noise to the points.
      point2D_refrac.x() += RandomGaussian(0.0, noise_level);
      point2D_refrac.y() += RandomGaussian(0.0, noise_level);
      point2D.x() += RandomGaussian(0.0, noise_level);
      point2D.y() += RandomGaussian(0.0, noise_level);
    } else {
      // Add huge noise to the points, this should be an outlier point.
      point2D_refrac.x() += RandomGaussian(0.0, 200.0);
      point2D_refrac.y() += RandomGaussian(0.0, 200.0);
      point2D.x() += RandomGaussian(0.0, 200.0);
      point2D.y() += RandomGaussian(0.0, 200.0);
    }

    points_data.points2D.push_back(point2D);
    points_data.points2D_refrac.push_back(point2D_refrac);
    points_data.points3D.push_back(point3D_world);
    cnt++;
  }

  camera.ComputeVirtuals(points_data.points2D_refrac,
                         points_data.virtual_cameras,
                         points_data.virtual_from_reals);

  // Generate a checkerboard like points data
  // const double square_size = 0.04;
  // for (size_t i = 0; i < 7; i++) {
  //   for (size_t j = 0; j < 8; j++) {
  //     Eigen::Vector3d point3D_world(static_cast<double>(i) * square_size,
  //                                   static_cast<double>(j) * square_size,
  //                                   0.0);

  //     // Project to image.
  //     const Eigen::Vector3d point3D_local = cam_from_world_gt *
  //     point3D_world; const Eigen::Vector2d point2D =
  //         camera.ImgFromCam(point3D_local.hnormalized());
  //     const Eigen::Vector2d point2D_refrac =
  //         camera.ImgFromCamRefrac(point3D_local);

  //     points_data.points2D.push_back(point2D);
  //     points_data.points2D_refrac.push_back(point2D_refrac);
  //     points_data.points3D.push_back(point3D_world);
  //   }
  // }
}

size_t EstimatePose(Camera& camera,
                    const PointsData& points_data,
                    Rigid3d& cam_from_world,
                    bool is_refractive) {
  // Only refine / estimate focal length, if no focal length was specified
  // (manually or through EXIF) and if it was not already estimated previously
  // from another image (when multiple images share the same camera
  // parameters)
  AbsolutePoseEstimationOptions abs_pose_options;
  abs_pose_options.num_threads = -1;
  abs_pose_options.num_focal_length_samples = 30;
  abs_pose_options.min_focal_length_ratio = 0.1;
  abs_pose_options.max_focal_length_ratio = 10.0;
  abs_pose_options.ransac_options.max_error = 12.0;
  abs_pose_options.ransac_options.min_inlier_ratio = 0.25;

  // Use high confidence to avoid preemptive termination of P3P RANSAC
  // - too early termination may lead to bad registration.
  abs_pose_options.ransac_options.min_num_trials = 100;
  abs_pose_options.ransac_options.max_num_trials = 10000;
  abs_pose_options.ransac_options.confidence = 0.99999;

  size_t num_inliers;
  std::vector<char> inlier_mask;

  if (!is_refractive) {
    if (!EstimateAbsolutePose(abs_pose_options,
                              points_data.points2D,
                              points_data.points3D,
                              &cam_from_world,
                              &camera,
                              &num_inliers,
                              &inlier_mask)) {
      std::cerr << "ERROR: Pose estimation failed" << std::endl;
      return 0;
    }

    if (num_inliers < 6) {
      std::cerr
          << "ERROR: Pose estimation failed, insufficient number of inliers"
          << std::endl;
      return 0;
    }
  } else {
    // Refractive pose estimation
    std::vector<size_t> camera_idxs(points_data.points2D_refrac.size());
    std::iota(camera_idxs.begin(), camera_idxs.end(), 0);

    if (!EstimateGeneralizedAbsolutePose(abs_pose_options.ransac_options,
                                         points_data.points2D_refrac,
                                         points_data.points3D,
                                         camera_idxs,
                                         points_data.virtual_from_reals,
                                         points_data.virtual_cameras,
                                         &cam_from_world,
                                         &num_inliers,
                                         &inlier_mask)) {
      std::cerr << "ERROR: Pose estimation failed" << std::endl;
      return 0;
    }

    if (num_inliers < 6) {
      std::cerr
          << "ERROR: Pose estimation failed, insufficient number of inliers"
          << std::endl;
      return 0;
    }
  }
  return num_inliers;
}

void PoseError(const Rigid3d& cam1_from_world,
               const Rigid3d& cam2_from_world,
               double& rotation_error,
               double& position_error) {
  Rigid3d cam2_from_cam1 = cam2_from_world * Inverse(cam1_from_world);
  rotation_error = RadToDeg(Eigen::AngleAxisd(cam2_from_cam1.rotation).angle());
  position_error =
      (cam2_from_cam1.rotation.inverse() * -cam2_from_cam1.translation).norm();
}

void Evaluate(Camera& camera,
              size_t num_points,
              size_t num_exps,
              double inlier_ratio,
              const std::string& output_path) {
  std::vector<double> noise_levels = {0.0, 0.2, 0.5, 0.8, 1.2, 1.5, 1.8, 2.0};

  std::ofstream file(output_path, std::ios::out);
  file << "# noise_level pos_error_mean pos_error_std rot_error_mean "
          "rot_error_std pos_error_refrac_mean pos_error_refrac_std "
          "rot_error_refrac_mean rot_error_refrac_std time time_refrac"
       << std::endl;

  for (const double& noise : noise_levels) {
    std::cout << "Noise level: " << noise << std::endl;

    // Generate random datasets first.
    std::vector<PointsData> datasets;
    datasets.reserve(num_exps);

    std::cout << "Generating random data ..." << std::endl;
    for (size_t i = 0; i < num_exps; i++) {
      // Create a random GT pose.
      const double qx = RandomUniformReal(0.0, 1.0);
      const double tx = RandomUniformReal(0.0, 1.0);

      const Rigid3d cam_from_world(
          Eigen::Quaterniond(1.0, qx, 0.0, 0.0).normalized(),
          Eigen::Vector3d(tx, 0.0, 0.0));

      PointsData points_data;
      GenerateRandom3D2DPoints(
          camera, num_points, cam_from_world, points_data, noise, inlier_ratio);
      datasets.push_back(points_data);
    }

    std::cout << "Evaluating ..." << std::endl;

    // Evaluate random dataset.
    std::vector<double> rotation_errors;
    std::vector<double> position_errors;
    std::vector<double> rotation_errors_refrac;
    std::vector<double> position_errors_refrac;

    // Inlier ratio
    std::vector<double> inlier_ratios;
    std::vector<double> inlier_ratios_refrac;

    double time = 0.0;
    double time_refrac = 0.0;

    Timer timer;

    // Perform in-air pose estimation
    timer.Start();
    for (size_t i = 0; i < num_exps; i++) {
      const PointsData& points_data = datasets[i];
      Rigid3d cam_from_world_est;
      size_t num_inliers =
          EstimatePose(camera, points_data, cam_from_world_est, false);

      double rotation_error, position_error;
      PoseError(points_data.cam_from_world_gt,
                cam_from_world_est,
                rotation_error,
                position_error);
      rotation_errors.push_back(rotation_error);
      position_errors.push_back(position_error);
      inlier_ratios.push_back(static_cast<double>(num_inliers) /
                              static_cast<double>(num_points));
    }
    timer.Pause();
    time = timer.ElapsedSeconds();

    timer.Restart();
    // Perform refractive pose estimation
    for (size_t i = 0; i < num_exps; i++) {
      const PointsData& points_data = datasets[i];
      Rigid3d cam_from_world_est_refrac;
      size_t num_inliers =
          EstimatePose(camera, points_data, cam_from_world_est_refrac, true);

      double rotation_error_refrac, position_error_refrac;
      PoseError(points_data.cam_from_world_gt,
                cam_from_world_est_refrac,
                rotation_error_refrac,
                position_error_refrac);
      rotation_errors_refrac.push_back(rotation_error_refrac);
      position_errors_refrac.push_back(position_error_refrac);
      inlier_ratios_refrac.push_back(static_cast<double>(num_inliers) /
                                     static_cast<double>(num_points));
    }
    timer.Pause();
    time_refrac = timer.ElapsedSeconds();

    const double pos_error_mean = Mean(position_errors);
    const double pos_error_std = StdDev(position_errors);
    const double rot_error_mean = Mean(rotation_errors);
    const double rot_error_std = StdDev(rotation_errors);

    const double pos_error_refrac_mean = Mean(position_errors_refrac);
    const double pos_error_refrac_std = StdDev(position_errors_refrac);
    const double rot_error_refrac_mean = Mean(rotation_errors_refrac);
    const double rot_error_refrac_std = StdDev(rotation_errors_refrac);

    const double inlier_ratio_mean = Mean(inlier_ratios);
    const double inlier_ratio_refrac_mean = Mean(inlier_ratios_refrac);

    file << noise << " " << pos_error_mean << " " << pos_error_std << " "
         << rot_error_mean << " " << rot_error_std << " "
         << pos_error_refrac_mean << " " << pos_error_refrac_std << " "
         << rot_error_refrac_mean << " " << rot_error_refrac_std << " " << time
         << " " << time_refrac << " " << inlier_ratio_mean << " "
         << inlier_ratio_refrac_mean << std::endl;
    std::cout << "Pose error in-air: Rotation: " << pos_error_mean << " +/- "
              << pos_error_std << " -- Position: " << rot_error_mean << " +/- "
              << rot_error_std << " -- inlier ratio: " << inlier_ratio_mean
              << " GT inlier ratio: " << inlier_ratio << std::endl;
    std::cout << "Pose error refrac: Rotation: " << pos_error_refrac_mean
              << " +/- " << pos_error_refrac_std
              << " -- Position: " << rot_error_refrac_mean << " +/- "
              << rot_error_refrac_std
              << " -- inlier ratio: " << inlier_ratio_refrac_mean
              << " GT inlier ratio: " << inlier_ratio << std::endl;
  }

  file.close();
}

int main(int argc, char* argv[]) {
  SetPRNGSeed(time(NULL));

  std::cout << "Setup some realistic camera model" << std::endl;

  // Camera parameters coming from Anton 131 map.
  //   Camera camera;
  //   camera.SetWidth(4104);
  //   camera.SetHeight(3006);
  //   camera.SetModelIdFromName("METASHAPE_FISHEYE");
  //   std::vector<double> params = {2115.7964068878537,
  //                                 2115.6679248258547,
  //                                 2097.4832274550317,
  //                                 1641.1207545881762,
  //                                 -0.0047743457655361719,
  //                                 0.053835104379748221,
  //                                 -0.086202869606942054,
  //                                 0.041314184080814435,
  //                                 0.00012198372904429465,
  //                                 0.00051849746014895923};
  Camera camera;
  camera.SetWidth(2048);
  camera.SetHeight(1536);
  camera.SetModelIdFromName("PINHOLE");
  std::vector<double> params = {
      1300.900000, 1300.900000, 1024.000000, 768.000000};
  camera.SetParams(params);

  // // Flatport setup.
  // camera.SetRefracModelIdFromName("FLATPORT");
  // Eigen::Vector3d int_normal;
  // int_normal[0] = RandomUniformReal(-0.3, 0.3);
  // int_normal[1] = RandomUniformReal(-0.3, 0.3);
  // int_normal[2] = RandomUniformReal(0.7, 1.3);

  // int_normal.normalize();

  // int_normal = Eigen::Vector3d::UnitZ();

  // std::vector<double> flatport_params = {int_normal[0],
  //                                        int_normal[1],
  //                                        int_normal[2],
  //                                        0.01,
  //                                        0.007,
  //                                        1.0,
  //                                        1.52,
  //                                        1.334};
  // camera.SetRefracParams(flatport_params);

  // Domeport setup.
  camera.SetRefracModelIdFromName("DOMEPORT");
  Eigen::Vector3d decentering;
  decentering[0] = RandomUniformReal(-0.003, 0.003);
  decentering[1] = RandomUniformReal(-0.003, 0.003);
  decentering[2] = RandomUniformReal(-0.01, 0.01);
  ;

  std::vector<double> domeport_params = {decentering[0],
                                         decentering[1],
                                         decentering[2],
                                         0.05,
                                         0.007,
                                         1.0,
                                         1.52,
                                         1.334};
  camera.SetRefracParams(domeport_params);

  // Generate simulated point data.
  const size_t num_points = 200;
  const double inlier_ratio = 0.7;

  std::string output_dir =
      "/home/mshe/workspace/omv_src/colmap-project/refrac_sfm_eval/data/"
      "abs_pose/";
  std::stringstream ss;
  ss << output_dir << "/eval_refrac_abs_pose_domeport_num_points_" << num_points
     << "_inlier_ratio_" << inlier_ratio << ".txt";
  std::string output_path = ss.str();

  Evaluate(camera, num_points, 1000, inlier_ratio, output_path);

  return true;
}