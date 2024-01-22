#include "colmap/estimators/cost_functions.h"
#include "colmap/estimators/generalized_pose.h"
#include "colmap/estimators/two_view_geometry.h"
#include "colmap/geometry/pose.h"
#include "colmap/geometry/rigid3.h"
#include "colmap/math/math.h"
#include "colmap/math/random.h"
#include "colmap/scene/camera.h"
#include "colmap/scene/projection.h"

#include <fstream>

using namespace colmap;

struct PointsData {
  std::vector<Eigen::Vector2d> points2D1;
  std::vector<Eigen::Vector2d> points2D1_refrac;
  std::vector<Eigen::Vector2d> points2D2;
  std::vector<Eigen::Vector2d> points2D2_refrac;

  // Store virtual cameras here.
  std::vector<Camera> virtual_cameras1;
  std::vector<Camera> virtual_cameras2;
  std::vector<Rigid3d> virtual_from_reals1;
  std::vector<Rigid3d> virtual_from_reals2;

  colmap::Rigid3d cam2_from_cam1_gt;
};

void GenerateRandomSecondViewPose(const Eigen::Vector3d& proj_center,
                                  const double distance,
                                  Rigid3d& cam2_from_cam1) {
  Eigen::Vector3d target(0.0, 0.0, distance);
  Eigen::Vector3d zaxis = (target - proj_center).normalized();
  Eigen::Vector3d yaxis(0, -1, 0);
  Eigen::Vector3d xaxis = (zaxis.cross(yaxis)).normalized();
  yaxis = (zaxis.cross(xaxis)).normalized();

  Eigen::Matrix3d R;
  R.col(0) = xaxis;
  R.col(1) = yaxis;
  R.col(2) = zaxis;
  Rigid3d cam1_from_cam2(Eigen::Quaterniond(R), proj_center);
  cam2_from_cam1 = Inverse(cam1_from_cam2);
}

void GenerateRandom2D2DPoints(const Camera& camera,
                              size_t num_points,
                              const Rigid3d& cam2_from_cam1_gt,
                              PointsData& points_data,
                              double noise_level,
                              double inlier_ratio) {
  // Generate refractive image points first, because flatport reduces FOV.

  points_data.points2D1.clear();
  points_data.points2D1_refrac.clear();
  points_data.points2D2.clear();
  points_data.points2D2_refrac.clear();

  points_data.points2D1.reserve(num_points);
  points_data.points2D1_refrac.reserve(num_points);
  points_data.points2D2.reserve(num_points);
  points_data.points2D2_refrac.reserve(num_points);

  points_data.cam2_from_cam1_gt = cam2_from_cam1_gt;

  size_t num_inliers =
      static_cast<size_t>(static_cast<double>(num_points) * inlier_ratio);
  size_t cnt = 0;
  while (true) {
    if (cnt >= num_points) {
      break;
    }
    Eigen::Vector2d point2D1_refrac;
    point2D1_refrac.x() =
        RandomUniformReal(0.5, static_cast<double>(camera.width) - 0.5);
    point2D1_refrac.y() =
        RandomUniformReal(0.5, static_cast<double>(camera.height) - 0.5);

    Ray3D ray_refrac = camera.CamFromImgRefrac(point2D1_refrac);

    const double d = RandomUniformReal(0.5, 2.5);

    // Now, do projection.
    Eigen::Vector3d point3D1 = ray_refrac.At(d);
    Eigen::Vector3d point3D2 = cam2_from_cam1_gt * point3D1;

    Eigen::Vector2d point2D2_refrac = camera.ImgFromCamRefrac(point3D2);

    if (std::isnan(point2D2_refrac.x()) || std::isnan(point2D2_refrac.y())) {
      continue;
    }

    if (point2D2_refrac.x() < 0 || point2D2_refrac.x() > camera.width ||
        point2D2_refrac.y() < 0 || point2D2_refrac.y() > camera.height) {
      continue;
    }

    Eigen::Vector2d point2D1 = camera.ImgFromCam(point3D1.hnormalized());
    Eigen::Vector2d point2D2 = camera.ImgFromCam(point3D2.hnormalized());

    if (cnt < num_inliers) {
      // Add noise to the points.
      if (noise_level > 0) {
        point2D1.x() += RandomGaussian(0.0, noise_level);
        point2D1.y() += RandomGaussian(0.0, noise_level);
        point2D1_refrac.x() += RandomGaussian(0.0, noise_level);
        point2D1_refrac.y() += RandomGaussian(0.0, noise_level);

        point2D2.x() += RandomGaussian(0.0, noise_level);
        point2D2.y() += RandomGaussian(0.0, noise_level);
        point2D2_refrac.x() += RandomGaussian(0.0, noise_level);
        point2D2_refrac.y() += RandomGaussian(0.0, noise_level);
      }
    } else {
      // Add huge noise to the points, this should be an outlier point.
      point2D1.x() += RandomGaussian(0.0, 200.0);
      point2D1.y() += RandomGaussian(0.0, 200.0);
      point2D1_refrac.x() += RandomGaussian(0.0, 200.0);
      point2D1_refrac.y() += RandomGaussian(0.0, 200.0);

      point2D2.x() += RandomGaussian(0.0, 200.0);
      point2D2.y() += RandomGaussian(0.0, 200.0);
      point2D2_refrac.x() += RandomGaussian(0.0, 200.0);
      point2D2_refrac.y() += RandomGaussian(0.0, 200.0);
    }

    points_data.points2D1.push_back(point2D1);
    points_data.points2D2.push_back(point2D2);
    points_data.points2D1_refrac.push_back(point2D1_refrac);
    points_data.points2D2_refrac.push_back(point2D2_refrac);

    cnt++;
  }

  camera.ComputeVirtuals(points_data.points2D1_refrac,
                         points_data.virtual_cameras1,
                         points_data.virtual_from_reals1);
  camera.ComputeVirtuals(points_data.points2D2_refrac,
                         points_data.virtual_cameras2,
                         points_data.virtual_from_reals2);
}

Camera BestFitNonRefracCamera(CameraModelId tgt_model_id,
                              const Camera& camera,
                              double tgt_distance) {
  Camera tgt_camera = Camera::CreateFromModelId(camera.camera_id,
                                                tgt_model_id,
                                                camera.MeanFocalLength(),
                                                camera.width,
                                                camera.height);

  // Sample 2D-3D correspondences for optimization.
  // Sample 2D-3D correspondences for optimization
  const size_t kNumSamples = 1000;
  std::vector<Eigen::Vector2d> points2D(kNumSamples);
  std::vector<Eigen::Vector3d> points3D(kNumSamples);
  const double width = static_cast<double>(tgt_camera.width);
  const double height = static_cast<double>(tgt_camera.height);

  for (size_t i = 0; i < kNumSamples; ++i) {
    const double depth = RandomGaussian(tgt_distance, 1.0);
    Eigen::Vector2d image_point(RandomUniformReal(0.5, width - 0.5),
                                RandomUniformReal(0.5, height - 0.5));
    Eigen::Vector3d world_point =
        camera.CamFromImgRefracPoint(image_point, depth);
    points2D[i] = image_point;
    points3D[i] = world_point;
  }

  const Rigid3d identity_pose = Rigid3d();

  ceres::Problem problem;
  ceres::Solver::Summary summary;
  ceres::Solver::Options solver_options;
  double* camera_params = tgt_camera.params.data();

  for (size_t i = 0; i < kNumSamples; i++) {
    const Eigen::Vector2d& point2D = points2D[i];
    Eigen::Vector3d& point3D = points3D[i];

    ceres::CostFunction* cost_function = nullptr;
    switch (tgt_camera.model_id) {
#define CAMERA_MODEL_CASE(CameraModel)                                        \
  case CameraModel::model_id:                                                 \
    cost_function = ReprojErrorConstantPoseCostFunction<CameraModel>::Create( \
        identity_pose, point2D);                                              \
    break;

      CAMERA_MODEL_SWITCH_CASES

#undef CAMERA_MODEL_CASE
    }

    problem.AddResidualBlock(
        cost_function, nullptr, point3D.data(), camera_params);

    problem.SetParameterBlockConstant(point3D.data());
  }

  solver_options.minimizer_progress_to_stdout = false;
  ceres::Solve(solver_options, &problem, &summary);

  // if optimization failed, use the original parameters
  if (!summary.IsSolutionUsable() ||
      summary.termination_type == ceres::TerminationType::NO_CONVERGENCE) {
    LOG(WARNING) << "Failed to compute the best fit persepctive model, taking "
                    "the original intrinsic parameters";
    tgt_camera.model_id = camera.model_id;
    tgt_camera.params = camera.params;
  } else {
    // Check for residuals.
    double reproj_error_sum = 0.0;
    for (size_t i = 0; i < kNumSamples; i++) {
      const double squared_reproj_error = CalculateSquaredReprojectionError(
          points2D[i], points3D[i], identity_pose, tgt_camera, false);
      reproj_error_sum += std::sqrt(squared_reproj_error);
    }
    reproj_error_sum = reproj_error_sum / static_cast<double>(kNumSamples);
    LOG(INFO) << "Best model " << tgt_camera.ModelName()
              << " computed, residual: " << reproj_error_sum << std::endl;
  }
  return tgt_camera;
}

size_t EstimateRelativePose(Camera& camera,
                            const PointsData& points_data,
                            Rigid3d& cam2_from_cam1,
                            bool is_refractive) {
  size_t num_points = points_data.points2D1.size();
  size_t num_inliers = 0;

  TwoViewGeometryOptions two_view_geometry_options;
  two_view_geometry_options.compute_relative_pose = true;
  two_view_geometry_options.ransac_options.max_error = 4.0;

  FeatureMatches matches;
  matches.reserve(num_points);

  for (size_t i = 0; i < num_points; i++) {
    matches.emplace_back(i, i);
  }

  TwoViewGeometry two_view_geometry;

  if (!is_refractive) {
    two_view_geometry =
        EstimateCalibratedTwoViewGeometry(camera,
                                          points_data.points2D1,
                                          camera,
                                          points_data.points2D2,
                                          matches,
                                          two_view_geometry_options);
    cam2_from_cam1 = two_view_geometry.cam2_from_cam1;
    num_inliers = two_view_geometry.inlier_matches.size();
  } else {
    // Refractive case.
    two_view_geometry_options.compute_relative_pose = true;
    two_view_geometry =
        EstimateRefractiveTwoViewGeometry(points_data.points2D1_refrac,
                                          points_data.virtual_cameras1,
                                          points_data.virtual_from_reals1,
                                          points_data.points2D2_refrac,
                                          points_data.virtual_cameras2,
                                          points_data.virtual_from_reals2,
                                          matches,
                                          two_view_geometry_options,
                                          true);
    cam2_from_cam1 = two_view_geometry.cam2_from_cam1;
    num_inliers = two_view_geometry.inlier_matches.size();
  }

  cam2_from_cam1 = two_view_geometry.cam2_from_cam1;
  num_inliers = two_view_geometry.inlier_matches.size();

  return num_inliers;
}

void RelativePoseError(const colmap::Rigid3d& cam2_from_cam1_gt,
                       const colmap::Rigid3d& cam2_from_cam1_est,
                       double& rotation_error,
                       double& angular_error,
                       double& scale_error,
                       bool is_refractive) {
  colmap::Rigid3d cam2_from_cam1_gt_norm = cam2_from_cam1_gt;
  cam2_from_cam1_gt_norm.translation.normalize();
  colmap::Rigid3d cam2_from_cam1_est_norm = cam2_from_cam1_est;
  cam2_from_cam1_est_norm.translation.normalize();

  Eigen::Quaterniond rotation_diff =
      cam2_from_cam1_gt.rotation * cam2_from_cam1_est.rotation.inverse();
  rotation_error = colmap::RadToDeg(Eigen::AngleAxisd(rotation_diff).angle());

  double cos_theta = cam2_from_cam1_gt_norm.translation.dot(
      cam2_from_cam1_est_norm.translation);
  if (cos_theta < 0) {
    cos_theta = -cos_theta;
  }
  if (cos_theta > 1) {
    cos_theta = 1.0;
  }
  angular_error = RadToDeg(acos(cos_theta));

  if (is_refractive) {
    const double baseline_est = (cam2_from_cam1_est.rotation.inverse() *
                                 -cam2_from_cam1_est.translation)
                                    .norm();
    const double baseline_gt =
        (cam2_from_cam1_gt.rotation.inverse() * -cam2_from_cam1_gt.translation)
            .norm();
    scale_error = (1 - baseline_est / baseline_gt);
  } else {
    scale_error = 0.0;
  }
}

void Evaluate(colmap::Camera& camera,
              size_t num_points,
              size_t num_exps,
              double inlier_ratio,
              const std::string& output_path) {
  std::vector<double> noise_levels = {0.0, 0.2, 0.5, 0.8, 1.2, 1.5, 1.8, 2.0};
  // std::vector<double> noise_levels = {0.0};

  std::ofstream file(output_path, std::ios::out);
  file << "# noise_level rot_error_mean rot_error_std angular_error_mean "
          "angular_error_std rot_error_refrac_mean rot_error_refrac_std "
          "angular_error_refrac_mean angular_error_refrac_std scale_error_mean "
          "scale_error_std scale_error_refrac_mean scale_error_refrac_std time "
          "time_refrac"
       << std::endl;

  for (const double& noise : noise_levels) {
    std::cout << "Noise level: " << noise << std::endl;

    // Generate random datasets first.
    std::vector<PointsData> datasets;
    datasets.reserve(num_exps);

    std::cout << "Generating random data ..." << std::endl;
    for (size_t i = 0; i < num_exps; i++) {
      // Create a random GT pose.
      const double tx = colmap::RandomUniformReal(-5.0, 5.0);
      const double ty = colmap::RandomUniformReal(-2.5, 2.5);
      const double tz = colmap::RandomUniformReal(-2.5, 2.5);
      const double distance = colmap::RandomUniformReal(0.5, 2.5);
      Eigen::Vector3d proj_center(tx, ty, tz);

      colmap::Rigid3d cam2_from_cam1;

      GenerateRandomSecondViewPose(proj_center, distance, cam2_from_cam1);
      PointsData points_data;
      GenerateRandom2D2DPoints(
          camera, num_points, cam2_from_cam1, points_data, noise, inlier_ratio);
      datasets.push_back(points_data);
    }

    // Evaluate random dataset.
    std::vector<double> rotation_errors;
    std::vector<double> angular_errors;
    std::vector<double> scale_errors;
    std::vector<double> rotation_errors_refrac;
    std::vector<double> angular_errors_refrac;
    std::vector<double> scale_errors_refrac;

    // Inlier ratio
    std::vector<double> inlier_ratios;
    std::vector<double> inlier_ratios_refrac;

    double time = 0.0;
    double time_refrac = 0.0;

    std::cout << "Evaluating non-refractive ..." << std::endl;

    Timer timer;

    // Perform non-refractive pose estimation
    timer.Start();
    for (size_t i = 0; i < num_exps; i++) {
      const PointsData& points_data = datasets[i];
      colmap::Rigid3d cam2_from_cam1_est;
      size_t num_inliers =
          EstimateRelativePose(camera, points_data, cam2_from_cam1_est, false);

      double rotation_error, angular_error, scale_error;
      RelativePoseError(points_data.cam2_from_cam1_gt,
                        cam2_from_cam1_est,
                        rotation_error,
                        angular_error,
                        scale_error,
                        false);
      rotation_errors.push_back(rotation_error);
      angular_errors.push_back(angular_error);
      scale_errors.push_back(scale_error);
      inlier_ratios.push_back(static_cast<double>(num_inliers) /
                              static_cast<double>(num_points));
    }
    timer.Pause();
    time = timer.ElapsedSeconds();

    std::cout << "Evaluating refractive ..." << std::endl;

    timer.Restart();
    // Perform refractive pose estimation
    for (size_t i = 0; i < num_exps; i++) {
      const PointsData& points_data = datasets[i];
      colmap::Rigid3d cam2_from_cam1_est_refrac;
      size_t num_inliers = EstimateRelativePose(
          camera, points_data, cam2_from_cam1_est_refrac, true);

      double rotation_error_refrac, angular_error_refrac, scale_error_refrac;
      // cam2_from_cam1_est_refrac.translation.normalize();
      RelativePoseError(points_data.cam2_from_cam1_gt,
                        cam2_from_cam1_est_refrac,
                        rotation_error_refrac,
                        angular_error_refrac,
                        scale_error_refrac,
                        true);
      rotation_errors_refrac.push_back(rotation_error_refrac);
      angular_errors_refrac.push_back(angular_error_refrac);
      scale_errors_refrac.push_back(scale_error_refrac);
      inlier_ratios_refrac.push_back(static_cast<double>(num_inliers) /
                                     static_cast<double>(num_points));
    }
    timer.Pause();
    time_refrac = timer.ElapsedSeconds();

    const double rot_error_mean = Mean(rotation_errors);
    const double rot_error_std = StdDev(rotation_errors);
    const double ang_error_mean = Mean(angular_errors);
    const double ang_error_std = StdDev(angular_errors);
    const double scale_error_mean = Mean(scale_errors);
    const double scale_error_std = StdDev(scale_errors);

    const double rot_error_refrac_mean = Mean(rotation_errors_refrac);
    const double rot_error_refrac_std = StdDev(rotation_errors_refrac);
    const double ang_error_refrac_mean = Mean(angular_errors_refrac);
    const double ang_error_refrac_std = StdDev(angular_errors_refrac);
    const double scale_error_refrac_mean = Mean(scale_errors_refrac);
    const double scale_error_refrac_std = StdDev(scale_errors_refrac);

    const double inlier_ratio_mean = colmap::Mean(inlier_ratios);
    const double inlier_ratio_refrac_mean = colmap::Mean(inlier_ratios_refrac);

    file << noise << " " << rot_error_mean << " " << rot_error_std << " "
         << ang_error_mean << " " << ang_error_std << " "
         << rot_error_refrac_mean << " " << rot_error_refrac_std << " "
         << ang_error_refrac_mean << " " << ang_error_refrac_std << " "
         << scale_error_mean << " " << scale_error_std << " "
         << scale_error_refrac_mean << " " << scale_error_refrac_std << " "
         << time << " " << time_refrac << " " << inlier_ratio_mean << " "
         << inlier_ratio_refrac_mean << std::endl;
    std::cout << "Relative pose error non-refrac: Rotation: " << rot_error_mean
              << " +/- " << rot_error_std << " -- Angular: " << ang_error_mean
              << " +/- " << ang_error_std << " -- Scale: " << scale_error_mean
              << " +/- " << scale_error_std
              << " -- inlier ratio: " << inlier_ratio_mean
              << " GT inlier ratio: " << inlier_ratio << std::endl;
    std::cout << "Relative pose error     refrac: Rotation: "
              << rot_error_refrac_mean << " +/- " << rot_error_refrac_std
              << " -- Angular: " << ang_error_refrac_mean << " +/- "
              << ang_error_refrac_std
              << " -- Scale: " << scale_error_refrac_mean << " +/- "
              << scale_error_refrac_std
              << " -- inlier ratio: " << inlier_ratio_refrac_mean
              << " GT inlier ratio: " << inlier_ratio << std::endl;
  }

  file.close();
}

int main(int argc, char* argv[]) {
  SetPRNGSeed(time(NULL));

  // Camera parameters coming from Anton 131 map.
  // Camera camera;
  // camera.SetWidth(4104);
  // camera.SetHeight(3006);
  // camera.SetModelIdFromName("METASHAPE_FISHEYE");
  // std::vector<double> params = {1000.7964068878537,
  //                               1000.6679248258547,
  //                               2097.4832274550317,
  //                               1641.1207545881762,
  //                               0,
  //                               0,
  //                               0,
  //                               0,
  //                               0,
  //                               0};
  Camera camera;
  camera.width = 1113;
  camera.height = 835;
  camera.model_id = CameraModelId::kPinhole;
  std::vector<double> params = {
      340.51429943677715, 340.51429943677715, 556.5, 417.5};
  camera.params = params;

  // Flatport setup.
  camera.refrac_model_id = CameraRefracModelId::kFlatPort;
  Eigen::Vector3d int_normal;
  int_normal[0] = RandomUniformReal(-0.2, 0.2);
  int_normal[1] = RandomUniformReal(-0.2, 0.2);
  int_normal[2] = RandomUniformReal(0.8, 1.2);

  int_normal.normalize();
  // int_normal = Eigen::Vector3d::UnitZ();

  std::vector<double> flatport_params = {int_normal[0],
                                         int_normal[1],
                                         int_normal[2],
                                         0.01,
                                         0.02,
                                         1.0,
                                         1.52,
                                         1.334};
  camera.refrac_params = flatport_params;

  // camera.SetRefracModelIdFromName("DOMEPORT");
  // Eigen::Vector3d decentering;
  // // decentering[0] = RandomUniformReal(-0.001, 0.001);
  // // decentering[1] = RandomUniformReal(-0.001, 0.001);
  // // decentering[2] = RandomUniformReal(-0.002, 0.002);

  // decentering[0] = 0;
  // decentering[1] = 0;
  // decentering[2] = 0;

  // std::vector<double> domeport_params = {decentering[0],
  //                                        decentering[1],
  //                                        decentering[2],
  //                                        0.05,
  //                                        0.007,
  //                                        1.0,
  //                                        1.52,
  //                                        1.334};
  // camera.SetRefracParams(domeport_params);

  Camera best_fit = BestFitNonRefracCamera(CameraModelId::kOpenCV, camera, 5.0);

  // Generate simulated point data.
  // const size_t num_points = 200;
  // const double inlier_ratio = 0.7;

  // std::string output_dir =
  //     "/home/mshe/workspace/omv_src/colmap-project/refrac_sfm_eval/plots/"
  //     "rel_pose/";
  // std::stringstream ss;
  // ss << output_dir << "/rel_pose_flat_non_ortho_close_num_points_" <<
  // num_points
  //    << "_inlier_ratio_" << inlier_ratio << ".txt";
  // std::string output_path = ss.str();

  // Evaluate(camera, num_points, 1000, inlier_ratio, output_path);

  return true;
}