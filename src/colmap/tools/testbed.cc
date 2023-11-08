#include "colmap/geometry/rigid3.h"
#include "colmap/math/random.h"
#include "colmap/scene/database_cache.h"
#include "colmap/scene/reconstruction.h"
#include "colmap/util/logging.h"

using namespace colmap;

int main(int argc, char* argv[]) {
  if (true) {
    // For David, add additional images to the reconstruction.
    const std::string input_path =
        "/data2/mshe/omv_src/colmap-project/dataset/2023-08_AL-Daycruise/"
        "2023-08-10_Alkor_0001_GMR_PFM-109_AUV-LUISE_Mission-307/"
        "reconstruction_subset/result/exp1/sparse/0/";
    const std::string database_path =
        "/data2/mshe/omv_src/colmap-project/dataset/2023-08_AL-Daycruise/"
        "2023-08-10_Alkor_0001_GMR_PFM-109_AUV-LUISE_Mission-307/"
        "reconstruction_subset/result/database.db";
    const std::string output_path =
        "/data2/mshe/omv_src/colmap-project/dataset/2023-08_AL-Daycruise/"
        "2023-08-10_Alkor_0001_GMR_PFM-109_AUV-LUISE_Mission-307/"
        "reconstruction_subset/result/exp1/for_david/sparse/";

    Rigid3d prior_from_cam(
        Eigen::Quaterniond(0.711987, -0.00218027, -0.00757204, 0.702149),
        Eigen::Vector3d(0.347714, 0.0330715, -0.529309));

    Rigid3d cam_from_prior = Inverse(prior_from_cam);

    Reconstruction recon;
    recon.Read(input_path);

    Database database(database_path);

    std::vector<Image> images = database.ReadAllImages();

    std::cout << "Current reconstruction: " << std::endl;
    std::cout << "Number of registered images: " << recon.NumRegImages()
              << std::endl;

    // Extra images I want to manually add:
    std::unordered_set<image_t> extra_image_ids = {
        1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
        14,  15,  16,  17,  18,  290, 291, 292, 293, 294, 295, 296, 297,
        298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308};

    // extra_image_ids = {};
    std::cout << "Extra image ids: ";
    for (image_t id : extra_image_ids) {
      std::cout << id << " ";
    }
    std::cout << std::endl;

    // Take the absolute navigation pose and register them in the
    // reconstruction.

    // for (const auto image_id : extra_image_ids) {
    //   Image image_db = database.ReadImage(image_id);

    //   const Rigid3d prior_from_world = image_db.CamFromWorldPrior();
    //   const Rigid3d cam_from_world_prior = cam_from_prior * prior_from_world;

    //   image_db.CamFromWorld() = cam_from_world_prior;
    //   image_db.SetRegistered(true);
    //   recon.AddImage(image_db);
    // }

    // Take the relative navigation pose and register them in the
    // reconstruction.

    while (!extra_image_ids.empty()) {
      image_t reg_image_id = 0;
      image_t ref_image_id = 0;
      for (image_t image_id : extra_image_ids) {
        if (recon.ExistsImage(image_id - 1) &&
            recon.IsImageRegistered(image_id - 1)) {
          ref_image_id = image_id - 1;
          reg_image_id = image_id;
          break;
        }
        if (recon.ExistsImage(image_id + 1) &&
            recon.IsImageRegistered(image_id + 1)) {
          ref_image_id = image_id + 1;
          reg_image_id = image_id;
          break;
        }
      }
      if (reg_image_id == ref_image_id || reg_image_id == 0 ||
          ref_image_id == 0) {
        std::cout << "No neighbor images found in the reconstruction"
                  << std::endl;
      } else {
        std::cout << "Registering image " << reg_image_id
                  << ", the reference image is: " << ref_image_id << std::endl;

        Image image_reg_db = database.ReadImage(reg_image_id);
        Image image_ref_db = database.ReadImage(ref_image_id);
        const Image& image_ref = recon.Image(ref_image_id);

        const Rigid3d& reg_prior_from_world = image_reg_db.CamFromWorldPrior();
        const Rigid3d& ref_prior_from_world = image_ref_db.CamFromWorldPrior();

        const Rigid3d reg_from_ref =
            reg_prior_from_world * Inverse(ref_prior_from_world);

        const Rigid3d& ref_cam_from_world = image_ref.CamFromWorld();

        image_reg_db.CamFromWorld() = reg_from_ref * ref_cam_from_world;
        image_reg_db.SetRegistered(true);
        recon.AddImage(image_reg_db);
        recon.RegisterImage(reg_image_id);

        std::cout << "Number of registered images: " << recon.NumRegImages()
                  << std::endl;

        extra_image_ids.erase(reg_image_id);
      }
    }
    recon.WriteText(output_path);
  }

  if (false) {
    Camera camera;
    camera.SetWidth(2048);
    camera.SetHeight(1536);
    camera.SetModelIdFromName("PINHOLE");
    std::vector<double> params = {
        1300.900000, 1300.900000, 1024.000000, 768.000000};
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
                                           0.01,
                                           0.014,
                                           1.0,
                                           1.52,
                                           1.334};
    camera.SetRefracParams(flatport_params);

    for (int i = 0; i < 10; i++) {
      const double x = RandomUniformReal(
          0.5, 10.0 /*static_cast<double>(camera.Width()) - 0.5*/);
      const double y = RandomUniformReal(
          0.5, 10.0 /*static_cast<double>(camera.Height()) - 0.5*/);

      Eigen::Vector2d point2D(x, y);
      Ray3D ray_refrac = camera.CamFromImgRefrac(point2D);
      const Eigen::Vector3d virtual_cam_center =
          camera.VirtualCameraCenter(ray_refrac);

      std::cout << "point: " << point2D.transpose()
                << ", virtual cam center: " << virtual_cam_center.transpose()
                << std::endl;
    }
  }

  if (false) {
    Eigen::Vector3d int_normal;
    int_normal[0] = RandomUniformReal(-0.3, 0.3);
    int_normal[1] = RandomUniformReal(-0.3, 0.3);
    int_normal[2] = RandomUniformReal(-0.7, 1.3);
    int_normal.normalize();
    std::cout << "int nromal: " << int_normal.transpose() << std::endl;
  }

  if (false) {
    // Create a reconstruction directly from navigation
    const std::string database_path =
        "/data2/mshe/omv_src/colmap-project/dataset/2023-08_AL-Daycruise/"
        "2023-08-10_Alkor_0001_GMR_PFM-109_AUV-LUISE_Mission-307/"
        "reconstruction/result/database.db";
    const std::string output_path =
        "/data2/mshe/omv_src/colmap-project/dataset/2023-08_AL-Daycruise/"
        "2023-08-10_Alkor_0001_GMR_PFM-109_AUV-LUISE_Mission-307/"
        "reconstruction/result/navigation/";

    Rigid3d prior_from_cam(
        Eigen::Quaterniond(0.711987, -0.00218027, -0.00757204, 0.702149),
        Eigen::Vector3d(0.347714, 0.0330715, -0.529309));

    Rigid3d cam_from_prior = Inverse(prior_from_cam);

    Database database(database_path);

    std::shared_ptr<DatabaseCache> database_cache =
        DatabaseCache::Create(database, 0, true, {});

    Reconstruction reconstruction;
    reconstruction.Load(*database_cache.get());

    for (const auto& image_it : reconstruction.Images()) {
      Image& image = reconstruction.Image(image_it.first);

      image.CamFromWorld() = cam_from_prior * image.CamFromWorldPrior();

      reconstruction.RegisterImage(image_it.first);
    }
    reconstruction.Write(output_path);
  }

  return true;
}