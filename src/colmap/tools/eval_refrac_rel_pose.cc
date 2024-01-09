#include "colmap/estimators/generalized_pose.h"
#include "colmap/estimators/two_view_geometry.h"
#include "colmap/geometry/pose.h"
#include "colmap/geometry/rigid3.h"
#include "colmap/math/math.h"
#include "colmap/math/random.h"
#include "colmap/scene/camera.h"

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
        RandomUniformReal(0.5, static_cast<double>(camera.Width()) - 0.5);
    point2D1_refrac.y() =
        RandomUniformReal(0.5, static_cast<double>(camera.Height()) - 0.5);

    Ray3D ray_refrac = camera.CamFromImgRefrac(point2D1_refrac);

    const double d = RandomUniformReal(0.5, 10.0);

    // Now, do projection.
    Eigen::Vector3d point3D1 = ray_refrac.At(d);
    Eigen::Vector3d point3D2 = cam2_from_cam1_gt * point3D1;

    Eigen::Vector2d point2D2_refrac = camera.ImgFromCamRefrac(point3D2);

    if (std::isnan(point2D2_refrac.x()) || std::isnan(point2D2_refrac.y())) {
      continue;
    }

    if (point2D2_refrac.x() < 0 || point2D2_refrac.x() > camera.Width() ||
        point2D2_refrac.y() < 0 || point2D2_refrac.y() > camera.Height()) {
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

void PoseError(const Rigid3d& cam2_from_cam1_gt,
               const Rigid3d& cam2_from_cam1_est,
               double& rotation_error,
               double& position_error,
               bool is_refractive) {
  if (!is_refractive) {
    colmap::Rigid3d cam2_from_cam1_gt_norm = cam2_from_cam1_gt;
    cam2_from_cam1_gt_norm.translation.normalize();

    colmap::Rigid3d diff =
        cam2_from_cam1_gt_norm * colmap::Inverse(cam2_from_cam1_est);
    rotation_error = colmap::RadToDeg(Eigen::AngleAxisd(diff.rotation).angle());
    // Position error in [mm]
    position_error = (Inverse(cam2_from_cam1_gt_norm).translation -
                      Inverse(cam2_from_cam1_est).translation)
                         .norm() *
                     1000.0;
  } else {
    colmap::Rigid3d diff =
        cam2_from_cam1_gt * colmap::Inverse(cam2_from_cam1_est);
    rotation_error = colmap::RadToDeg(Eigen::AngleAxisd(diff.rotation).angle());
    // Position error in [mm]
    position_error = (Inverse(cam2_from_cam1_gt).translation -
                      Inverse(cam2_from_cam1_est).translation)
                         .norm() *
                     1000.0;
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
  file << "# noise_level rot_error_mean rot_error_std pos_error_mean "
          "pos_error_std rot_error_refrac_mean rot_error_refrac_std "
          "pos_error_refrac_mean pos_error_refrac_std time time_refrac"
       << std::endl;

  for (const double& noise : noise_levels) {
    std::cout << "Noise level: " << noise << std::endl;

    // Generate random datasets first.
    std::vector<PointsData> datasets;
    datasets.reserve(num_exps);

    std::cout << "Generating random data ..." << std::endl;
    for (size_t i = 0; i < num_exps; i++) {
      // Create a random GT pose.
      double ry = colmap::RandomUniformReal(colmap::DegToRad(-30.0),
                                            colmap::DegToRad(30.0));
      double rx = colmap::RandomUniformReal(colmap::DegToRad(-30.0),
                                            colmap::DegToRad(30.0));
      double rz = colmap::RandomUniformReal(colmap::DegToRad(-30.0),
                                            colmap::DegToRad(30.0));
      double tx = colmap::RandomUniformReal(-2.0, 2.0);
      double ty = colmap::RandomUniformReal(-2.0, 2.0);
      double tz = colmap::RandomUniformReal(-2.0, 2.0);
      colmap::Rigid3d cam2_from_cam1;
      cam2_from_cam1.rotation =
          Eigen::Quaterniond(EulerAnglesToRotationMatrix(rx, ry, rz))
              .normalized();
      cam2_from_cam1.translation = Eigen::Vector3d(tx, ty, tz);

      PointsData points_data;
      GenerateRandom2D2DPoints(
          camera, num_points, cam2_from_cam1, points_data, noise, inlier_ratio);
      datasets.push_back(points_data);
    }

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

    std::cout << "Evaluating non-refractive ..." << std::endl;

    Timer timer;

    // Perform non-refractive pose estimation
    timer.Start();
    for (size_t i = 0; i < num_exps; i++) {
      const PointsData& points_data = datasets[i];
      colmap::Rigid3d cam2_from_cam1_est;
      size_t num_inliers =
          EstimateRelativePose(camera, points_data, cam2_from_cam1_est, false);

      double rotation_error, position_error;
      PoseError(points_data.cam2_from_cam1_gt,
                cam2_from_cam1_est,
                rotation_error,
                position_error,
                false);
      rotation_errors.push_back(rotation_error);
      position_errors.push_back(position_error);
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

      double rotation_error_refrac, position_error_refrac;
      cam2_from_cam1_est_refrac.translation.normalize();
      PoseError(points_data.cam2_from_cam1_gt,
                cam2_from_cam1_est_refrac,
                rotation_error_refrac,
                position_error_refrac,
                false);
      rotation_errors_refrac.push_back(rotation_error_refrac);
      position_errors_refrac.push_back(position_error_refrac);
      inlier_ratios_refrac.push_back(static_cast<double>(num_inliers) /
                                     static_cast<double>(num_points));
    }
    timer.Pause();
    time_refrac = timer.ElapsedSeconds();

    const double rot_error_mean = Mean(rotation_errors);
    const double rot_error_std = StdDev(rotation_errors);
    const double pos_error_mean = Mean(position_errors);
    const double pos_error_std = StdDev(position_errors);

    const double rot_error_refrac_mean = Mean(rotation_errors_refrac);
    const double rot_error_refrac_std = StdDev(rotation_errors_refrac);
    const double pos_error_refrac_mean = Mean(position_errors_refrac);
    const double pos_error_refrac_std = StdDev(position_errors_refrac);

    const double inlier_ratio_mean = colmap::Mean(inlier_ratios);
    const double inlier_ratio_refrac_mean = colmap::Mean(inlier_ratios_refrac);

    file << noise << " " << rot_error_mean << " " << rot_error_std << " "
         << pos_error_mean << " " << pos_error_std << " "
         << rot_error_refrac_mean << " " << rot_error_refrac_std << " "
         << pos_error_refrac_mean << " " << pos_error_refrac_std << " " << time
         << " " << time_refrac << " " << inlier_ratio_mean << " "
         << inlier_ratio_refrac_mean << std::endl;
    std::cout << "Pose error non-refrac: Rotation: " << rot_error_mean
              << " +/- " << rot_error_std << " -- Position: " << pos_error_mean
              << " +/- " << pos_error_std
              << " -- inlier ratio: " << inlier_ratio_mean
              << " GT inlier ratio: " << inlier_ratio << std::endl;
    std::cout << "Pose error     refrac: Rotation: " << rot_error_refrac_mean
              << " +/- " << rot_error_refrac_std
              << " -- Position: " << pos_error_refrac_mean << " +/- "
              << pos_error_refrac_std
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
  camera.SetWidth(1113);
  camera.SetHeight(835);
  camera.SetModelIdFromName("PINHOLE");
  std::vector<double> params = {
      340.51429943677715, 340.51429943677715, 556.5, 417.5};
  camera.SetParams(params);

  // Flatport setup.
  camera.SetRefracModelIdFromName("FLATPORT");
  Eigen::Vector3d int_normal;
  int_normal[0] = RandomUniformReal(-0.3, 0.3);
  int_normal[1] = RandomUniformReal(-0.3, 0.3);
  int_normal[2] = RandomUniformReal(0.7, 1.3);

  int_normal.normalize();
  // int_normal = Eigen::Vector3d::UnitZ();

  std::vector<double> flatport_params = {int_normal[0],
                                         int_normal[1],
                                         int_normal[2],
                                         0.05,
                                         0.02,
                                         1.0,
                                         1.52,
                                         1.334};
  camera.SetRefracParams(flatport_params);

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

  // Generate simulated point data.
  const size_t num_points = 2000;
  const double inlier_ratio = 1.0;

  std::string output_dir =
      "/home/mshe/workspace/omv_src/colmap-project/refrac_sfm_eval/plots/"
      "rel_pose/";
  std::stringstream ss;
  ss << output_dir << "/rel_pose_flat_non_ortho_far_num_points_" << num_points
     << "_inlier_ratio_" << inlier_ratio << ".txt";
  std::string output_path = ss.str();

  Evaluate(camera, num_points, 200, inlier_ratio, output_path);

  return true;
}