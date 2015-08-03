#include <boost/foreach.hpp>
#include <boost/scope_exit.hpp>

#include <ros/ros.h>

#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>
#include <hueblob/Blob.h>
#include <std_msgs/Int8.h>

#include <hueblob/AddObject.h>

#include "libhueblob/hueblob.hh"

#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <highgui.h>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <boost/format.hpp>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/centroid.h>
#include <pcl/common/common.h>
#include <pcl/features/feature.h>
#include "pcl/filters/statistical_outlier_removal.h"
#include <pcl_conversions/pcl_conversions.h>
//#include <pcl_visualization/cloud_viewer.h>
#include <Eigen/Dense>
#include <iostream>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

void nullDeleter(void*) {}
void nullDeleterConst(const void*) {}

struct YamlModel {
  std::string name;
  std::string path;
};

void operator >> (const YAML::Node& node, YamlModel& model) {
   model.name = node["name"].as<std::string>();
   model.path = node["path"].as<std::string>();
}


HueBlob::HueBlob()
  : nh_("hueblob"),
    it_(nh_),
    stereo_topic_prefix_ (),
    threshold_(),
    left_sub_(),
    right_sub_(),
    disparity_sub_(),
    exact_sync_(3),
    approximate_sync_(100),
    left_objects_(),
    right_objects_(),
    check_synced_timer_(),
    left_received_(),
    right_received_(),
    disp_received_(),
    all_received_(),
    leftImage_(),
    rightImage_(),
    leftCamera_(),
    disparity_(),
    preload_models_()
{
  // Parameter initialization.
  ros::param::param<std::string>("~stereo", stereo_topic_prefix_, "");
  ros::param::param<std::string>("~frame", frame_, "camera_bottom_left_optical");
  ros::param::param<std::string>("~models", preload_models_, "");
  ros::param::param<bool>("~approximate_sync", is_approximate_sync_, false);
  ros::param::param<double>("threshold", threshold_, 75.);

  const std::string tracked_image_topic =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/tracked/image_rect_color");
  tracked_left_pub_ = it_.advertise(tracked_image_topic, 1);

  // const std::string blobs_topic =
  //   ros::names::append("/hueblob/", stereo_topic_prefix_ + "/blobs");
  // blobs_pub_ = nh_.advertise<hueblob::Blobs>(blobs_topic, 5);

  const std::string count_topic =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/blobs/count");
  count_pub_ = nh_.advertise<std_msgs::Int8>(count_topic, 5);

  const std::string points2_topic =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/points2");
  cloud_pub_  = nh_.advertise<pcl::PointCloud<pcl::PointXYZ> > (points2_topic, 1);

  const std::string add_object_service =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/add_object");
  AddObject_srv_ = nh_.advertiseService(add_object_service,
					&HueBlob::AddObjectCallback, this);

  const std::string list_objects_service =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/list_objects");
  ListObject_srv_  = nh_.advertiseService(list_objects_service,
					  &HueBlob::ListObjectCallback, this);

  const std::string rm_object_service =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/rm_objects");
  RmObject_srv_ = nh_.advertiseService(rm_object_service,
				       &HueBlob::RmObjectCallback, this);

  const std::string track_object_service =
    ros::names::append("/hueblob/", stereo_topic_prefix_ + "/track_object");
  TrackObject_srv_ = nh_.advertiseService(track_object_service,
					  &HueBlob::TrackObjectCallback, this);

  // Initialize the node subscribers, publishers and filters.
  ROS_INFO("Setting up Infrastructure");
  setupInfrastructure(stereo_topic_prefix_);
}

HueBlob::~HueBlob()
{
  ROS_DEBUG("Destructing the node.");
}

void
HueBlob::publish_tracked_images(hueblob::Blobs blobs)
{
  //ROS_DEBUG_THROTTLE(1, "publish_track_cb");
  // static const std::string tracked_image_topic =
  //   ros::names::append("/hueblob/", stereo_topic_prefix_ + "/tracked/image_rect_color");
  if ( tracked_left_pub_.getNumSubscribers() == 0)
    {
      return;
    }

  cv_bridge::CvImagePtr img;
  bool tracked(false);
  for (  std::vector<hueblob::Blob>::iterator iter= blobs.blobs.begin();
         iter != blobs.blobs.end(); iter++ )
    {
      if (!leftImage_ || !rightImage_)
        break;
      tracked = true;
      img = cv_bridge::toCvCopy(leftImage_, "bgr8");
      int x =  (*iter).boundingbox_2d[0];
      int y =  (*iter).boundingbox_2d[1];
      int width =  (*iter).boundingbox_2d[2];
      int height =  (*iter).boundingbox_2d[3];
      cv::Point p1(x, y);
      cv::Point p2(x + width, y + height);
      cv::Point pc(x, y + std::max(16, height+8));
      const cv::Scalar color = CV_RGB(255,0,0);
      ROS_DEBUG_STREAM("Drawing rect " << x << " " << " " << y
                       << " " << width << " " << height);
      cv::rectangle(img->image, p1, p2, color, 1);
      std::stringstream ss (std::stringstream::in | std::stringstream::out);
      cv::putText(img->image, (*iter).name, p1, CV_FONT_HERSHEY_SIMPLEX,
                  0.5, color);

    }

  if (leftImage_ && tracked){
    cv_bridge::CvImage brd_im;
    brd_im.image = img->image;
    brd_im.header = leftImage_->header;
    brd_im.encoding = sensor_msgs::image_encodings::TYPE_8UC3;
    tracked_left_pub_.publish(brd_im.toImageMsg());
  }
  else
    ROS_WARN_THROTTLE(20,"leftImage_ is not received");

}

void
HueBlob::spin()
{
  // typedef std::pair<const std::string&, const Object&> iterator_t;

  // ros::Rate loop_rate(10);

  // ROS_INFO("Entering  main loop");
  // cv::Mat img;
  // while (ros::ok())
  //   {
  //     hueblob::Blobs blobs;
  //     BOOST_FOREACH(iterator_t it, left_objects_)
  //       {
  //         hueblob::Blob blob = trackBlob(it.first);
  //         blobs.blobs.push_back(blob);
  //       }

  //     // blobs_pub_.publish(blobs);
  //     publish_tracked_images(blobs);
  //     ros::spinOnce();
  //     loop_rate.sleep();
  //   }
}

namespace
{
  void increment(int* value)
  {
    ++(*value);
  }
} // end of anonymous namespace.

void
HueBlob::setupInfrastructure(const std::string& stereo_prefix)
{
  stereo_topic_prefix_ = nh_.resolveName(stereo_prefix);

  const std::string left_topic =
    ros::names::clean(stereo_topic_prefix_ + "/left/image_rect_color");
  const std::string left_camera_topic =
    ros::names::clean(stereo_topic_prefix_ + "/left/camera_info");
  const std::string right_topic =
    ros::names::clean(stereo_topic_prefix_ + "/right/image_rect_color");
  const std::string right_camera_topic =
    ros::names::clean(stereo_topic_prefix_ + "/right/camera_info");
  const std::string disparity_topic =
    ros::names::clean(stereo_topic_prefix_ + "/disparity");

  left_sub_.subscribe(it_, left_topic, 3);
  leftCamera_sub_.subscribe(nh_, left_camera_topic, 3);
  right_sub_.subscribe(it_, right_topic, 3);
  rightCamera_sub_.subscribe(nh_, right_camera_topic, 3);
  disparity_sub_.subscribe(nh_, disparity_topic, 3);

  if (is_approximate_sync_)
    {
      approximate_sync_.connectInput(left_sub_, leftCamera_sub_,
                                     right_sub_, rightCamera_sub_,
                                     disparity_sub_);
      approximate_sync_.registerCallback(boost::bind(&HueBlob::imageCallback,
				     this, _1, _2, _3, _4, _5));
      ROS_INFO("approximate_sync mode");
    }
  else
    {
      exact_sync_.connectInput(left_sub_, leftCamera_sub_,
                               right_sub_, rightCamera_sub_,
                               disparity_sub_);
      exact_sync_.registerCallback(boost::bind(&HueBlob::imageCallback,
				     this, _1, _2, _3, _4, _5));

      ROS_INFO("Starting in exact_sync mode");
      // Complain every 30s if the topics appear unsynchronized
      left_sub_.registerCallback(boost::bind(increment, &left_received_));
      right_sub_.registerCallback(boost::bind(increment, &right_received_));
      disparity_sub_.registerCallback(boost::bind(increment, &disp_received_));
      exact_sync_.registerCallback(boost::bind(increment, &all_received_));
      check_synced_timer_ =
        nh_.createWallTimer(ros::WallDuration(30.0),
			boost::bind(&HueBlob::checkInputsSynchronized, this));
    }
  ROS_INFO("Subscribing to:\n"
	   "\t* %s\n"
	   "\t* %s\n"
	   "\t* %s\n"
	   "\t* %s\n"
	   "\t* %s",
	   left_topic.c_str(), left_camera_topic.c_str(),
	   right_topic.c_str(), right_camera_topic.c_str(),
	   disparity_topic.c_str());
  if (preload_models_ != "")
    {
    try
      {
        /// @TODO support multiple models
        YAML::Node doc = YAML::LoadFile(preload_models_);
        for(unsigned i=0;i<doc.size();i++) {
          YamlModel yaml_model;
          doc[i] >> yaml_model;
          ROS_INFO_STREAM("Adding "<< yaml_model.name << " " << yaml_model.path);
            // Get reference on the object.

          Object& left_object = left_objects_[yaml_model.name];
          Object& right_object = right_objects_[yaml_model.name];
          const std::string blob_topic =
            ros::names::append("/hueblob/", stereo_topic_prefix_ + "/blobs/" + yaml_model.name);

          blob_pubs_[yaml_model.name] = nh_.advertise<hueblob::Blob>(blob_topic, 5);

          // Emit a warning if the object already exists.
          if (left_object.anchor_x_
              || left_object.anchor_y_
              || left_object.anchor_z_)
            ROS_WARN("Overwriting the object %s", yaml_model.name.c_str());
          cv::Mat model = cv::imread(yaml_model.path);
          // Add the view to the object.
          left_object.addView(model);
          // Add the view to the object.
          right_object.addView(model);
        }
      }
      catch(YAML::ParserException& e) {
        ROS_FATAL_STREAM(e.what());
      }


      ROS_INFO_STREAM("parsed models: "<< preload_models_);
    }

}

void
HueBlob::imageCallback(const sensor_msgs::ImageConstPtr& left,
		       const sensor_msgs::CameraInfoConstPtr& left_camera,
		       const sensor_msgs::ImageConstPtr& right,
		       const sensor_msgs::CameraInfoConstPtr& right_camera,
		       const stereo_msgs::DisparityImageConstPtr& disparity)
{
  leftImage_ = left;
  rightImage_ = right;
  leftCamera_ = left_camera;
  disparity_ = disparity;
  typedef std::pair<const std::string&, const Object&> iterator_t;
  unsigned count(0);
  hueblob::Blobs blobs;
  BOOST_FOREACH(iterator_t it, left_objects_)
    {
      hueblob::Blob blob = trackBlob(it.first);
      blobs.blobs.push_back(blob);
      blob_pubs_[blob.name].publish(blob);
      count++;
    }
  // blobs_pub_.publish(blobs);
  std_msgs::Int8 cnt;
  cnt.data = count;
  count_pub_.publish(cnt);
  publish_tracked_images(blobs);
}

bool
HueBlob::AddObjectCallback(hueblob::AddObject::Request& request,
			   hueblob::AddObject::Response& response)
{
  response.status = 0;

  // Convert ROS image to OpenCV.
  cv_bridge::CvImageConstPtr model_;
  try
    {
      boost::shared_ptr<sensor_msgs::Image> image_ptr
	(&request.image, nullDeleter);
      model_ = cv_bridge::toCvShare(image_ptr,"bgr8");
    }
  catch(const cv_bridge::Exception& error)
    {
      ROS_ERROR("failed to convert image");
      return false;
    }
  cv::Mat model(model_->image);


  // Get reference on the object.
  Object& left_object = left_objects_[request.name];
  Object& right_object = right_objects_[request.name];

  // Emit a warning if the object already exists.
  if (left_object.anchor_x_
      || left_object.anchor_y_
      || left_object.anchor_z_)
    ROS_WARN("Overwriting the object %s", request.name.c_str());

  // Initialize the object.
  left_object.anchor_x_ = request.anchor.x;
  left_object.anchor_y_ = request.anchor.y;
  left_object.anchor_z_ = request.anchor.z;
  // Add the view to the object.
  left_object.addView(model);

  right_object.anchor_x_ = request.anchor.x;
  right_object.anchor_y_ = request.anchor.y;
  right_object.anchor_z_ = request.anchor.z;
  // Add the view to the object.
  right_object.addView(model);

  return true;
}

bool
HueBlob::ListObjectCallback(hueblob::ListObject::Request& request,
			    hueblob::ListObject::Response& response)
{
  typedef std::pair<const std::string&, const Object&> iterator_t;
  BOOST_FOREACH(iterator_t it, left_objects_)
    response.objects.push_back(it.first);
  return true;
}

bool
HueBlob::RmObjectCallback(hueblob::RmObject::Request& request,
			  hueblob::RmObject::Response& response)
{
  left_objects_.erase(request.name);
  right_objects_.erase(request.name);
  return true;
}

bool
HueBlob::TrackObjectCallback(hueblob::TrackObject::Request& request,
			     hueblob::TrackObject::Response& response)
{
  return true;
}

namespace
{

  inline void projectTo3d(float u, float v, float disparity,
                          const stereo_msgs::DisparityImage &disparity_image,
                          const sensor_msgs::CameraInfo &camera_info,
                          float &x, float &y, float &z,
                          bool shift_correction = false)
  {

    float fx = camera_info.P[0*4+0];
    float fy = camera_info.P[1*4+1];
    float cx = camera_info.P[0*4+2];
    float cy = camera_info.P[1*4+2];
    // float Tx = camera_info.P[0*4+3];
    // float Ty = camera_info.P[1*4+3];

    // if (shift_correction)
    //   {
    //     // account for the fact the center point of the image is not the
    //     // principal point
    //     // not sure if this is really correct or more of a hack, but
    //     // without this the image
    //     // ends up shifted
    //     u = u - (disparity_image.image.width/2  - cx);
    //     v = v - (disparity_image.image.height/2 - cy);
    //   }

    z = disparity_image.f * disparity_image.T / disparity;
    x = ( (u - cx ) / fx );
    x*= z;
    y = ( (v - cy ) / fy );
    y*= z;
  }

  //! Check if a disparity image as a valid value at given image coordinates
  inline bool hasDisparityValue(const stereo_msgs::DisparityImage
                                &disparity_image, unsigned int h, unsigned int w)
  {
    if (h>= disparity_image.image.height && w >= disparity_image.image.width)
      return false;
    float val;
    memcpy(&val, &(disparity_image.image.data.at(
                                                 h*disparity_image.image.step
                                                 + sizeof(float)*w )), sizeof(float));
    if (std::isnan(val) || std::isinf(val)) return false;
    if (val < disparity_image.min_disparity || val >
        disparity_image.max_disparity) return false;
    return true;
  }
  //! Get the value from an image at given image coordinates as a float
  inline void getPoint(const sensor_msgs::Image &image, unsigned int h,
                       unsigned int w, float &val)
  {
    ROS_ASSERT(h<image.height && w<image.width);
    memcpy(&val, &(image.data.at( h*image.step + sizeof(float)*w )),
           sizeof(float));
  }


  void get3dCloud(const stereo_msgs::DisparityImage &disparity_image,
                  const sensor_msgs::CameraInfo &camera_info,
                  cv::Rect& rect,
                  cv::Rect& right_rect,
                  pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud,
                  cv::Point3f& center_est
                  )
  {
    cv::Point2f right_center(right_rect.x
                             + right_rect.width*0.5,
                             right_rect.y
                             + right_rect.height*0.5);
    cv::Point2f left_center(rect.x
                            + rect.width*0.5,
                            rect.y
                            + rect.height*0.5);

    // Make sure the rectangle is valid.
    // check if the size of two rect are not too different
    double diffy = double(right_center.y - left_center.y);
    if ( diffy > 10 || diffy < -10 ||
         (1.0*rect.width/float(right_rect.width)) > 1.5 ||
         (1.0*rect.width/float(right_rect.width)) < 0.5
        )
      {
        ROS_DEBUG_STREAM("object on left and right cam not aligned"
                         << " or too different in size"
                         << "\nright_center.y - left_center.y = " << diffy
                         );
        ROS_DEBUG_STREAM("\nleft: "  << left_center  << " "<< rect.width
                         << " " << rect.height
                         << "\nright: " << right_center << " "<< right_rect.width
                         << " " << right_rect.height
                         );
        center_est.x = 0;
        center_est.y = 0;
        center_est.z = 0;
      }
    else
      {
        //ROS_DEBUG_STREAM(right_center << " " << left_center << " ");
        float disparity = left_center.x - right_center.x;
        int i = rect.y;
        int j = rect.x;
        float x, y, z;
        projectTo3d(j, i, disparity,  disparity_image,
                    camera_info, x, y, z, true);
        center_est.x = x;
        center_est.y = y;
        center_est.z = z;
        // ROS_DEBUG_STREAM(left_center << " "
        //                  << right_center << " "
        //                  << center_est);
      }
    for (int i = rect.y; i < rect.y + rect.height; ++i)
      for (int j = rect.x; j < rect.x + rect.width; ++j)
        {
          if (!hasDisparityValue(disparity_image, i, j))
            continue;
          float disparity, x, y, z;
          getPoint(disparity_image.image, i, j, disparity);
          ROS_ASSERT(disparity_image.max_disparity != 0.0);
          if (disparity == 0)
            continue;
          projectTo3d(j, i, disparity,  disparity_image,
                      camera_info, x, y, z, true);
          pcl::PointXYZ point(x,y,z);
          pcl_cloud->points.push_back(point);
        }
  }

} // end of anonymous namespace.

hueblob::Blob
HueBlob::trackBlob(const std::string& name)
{
  hueblob::Blob blob;
  // Image acquisition.
  if (!leftImage_ || !disparity_ || !rightImage_)
    {
      ROS_WARN_STREAM_THROTTLE(1, "At least one of leftImage_ || disparity_ "
                               "|| rightImage_ is missing. Aborting tracking"
                               << leftImage_ << " " << rightImage_ << " "
                               << disparity_);
      return blob;
    }
  // Fill blob header.
  blob.name = name;
  blob.header = leftImage_->header;
  blob.position.header = leftImage_->header;
  blob.position.child_frame_id = "/hueblob_" + name;
  blob.boundingbox_2d.resize(4);
  for (unsigned i = 0; i < 4; ++i)
    blob.boundingbox_2d[i] = 0.;

  // Realize 2d tracking in the image.
  Object& robject = right_objects_[name];
  // get box for right image
  cv::Mat right_image(cv_bridge::toCvShare(rightImage_, "bgr8")->image);
  boost::optional<cv::RotatedRect> right_rrect = robject.track(right_image);

  Object& object = left_objects_[name];
  cv::Mat image(cv_bridge::toCvShare(leftImage_, "bgr8")->image);
  boost::optional<cv::RotatedRect> rrect = object.track(image);
  if (!rrect)
    {
      ROS_WARN_THROTTLE(20, "failed to track object");
      return blob;
    }

  cv::Rect rect = rrect->boundingRect();
  cv::Rect right_rect = right_rrect->boundingRect();

  blob.boundingbox_2d[0] = rect.x;
  blob.boundingbox_2d[1] = rect.y;
  blob.boundingbox_2d[2] = rect.width;
  blob.boundingbox_2d[3] = rect.height;

  if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0)
    {
      ROS_WARN_THROTTLE
        (20, "failed to track object (invalid tracking window)");
      return blob;
    }

  cv::Point3d center;
  // static pcl_visualization::CloudViewer viewer("Simple Cloud Viewer");
  pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered (new pcl::PointCloud<pcl::PointXYZ>);
  cv::Point3f center_est;
  get3dCloud(*disparity_, *leftCamera_,
             rect, right_rect,
             pcl_cloud, center_est);
  // std::cerr << "Cloud before filtering: " << std::endl;
  // std::cerr << *pcl_cloud << std::endl;
  float depth_density = 1.*pcl_cloud->points.size()/(rect.width*rect.height);

  Eigen::Vector4f centroid (0., 0., 0., 0.);
  Eigen::Vector4f min3d (0., 0., 0., 0.);
  Eigen::Vector4f max3d (0., 0., 0., 0.);
  if (pcl_cloud->points.size() >0)
    {
      //cloud_pub_.publish(pcl_cloud);
      pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
      sor.setInputCloud (pcl_cloud);
      sor.setMeanK (50);
      sor.setStddevMulThresh (1.0);
      sor.filter (*cloud_filtered);
      cloud_filtered->header.frame_id = frame_;
      pcl::uint64_t pcl_stamp_;
      pcl_conversions::toPCL(leftImage_->header.stamp, pcl_stamp_);
      cloud_filtered->header.stamp = pcl_stamp_;
      pcl::compute3DCentroid(*cloud_filtered, centroid);
      pcl::getMinMax3D(*cloud_filtered, min3d, max3d);
      cloud_pub_.publish(cloud_filtered);
      blob.boundingbox_3d[0] = min3d[0];
      blob.boundingbox_3d[1] = min3d[1];
      blob.boundingbox_3d[2] = min3d[2];
      blob.boundingbox_3d[3] = max3d[0];
      blob.boundingbox_3d[4] = max3d[1];
      blob.boundingbox_3d[5] = max3d[2];
    }
  // std::cerr << "Cloud after filtering: " << std::endl;
  // std::cerr << *cloud_filtered << std::endl;

  center.x = centroid[0];
  center.y = centroid[1];
  center.z = centroid[2];
  center.x += object.anchor_x_;
  center.y += object.anchor_y_;
  center.z += object.anchor_z_;

  // Fill blob.
  blob.cloud_centroid.transform.translation.x = center.x;
  blob.cloud_centroid.transform.translation.y = center.y;
  blob.cloud_centroid.transform.translation.z = center.z;
  blob.cloud_centroid.transform.rotation.x = 0.;
  blob.cloud_centroid.transform.rotation.y = 0.;
  blob.cloud_centroid.transform.rotation.z = 0.;
  blob.cloud_centroid.transform.rotation.w = 1.;
  blob.cloud_centroid.header.stamp = leftImage_->header.stamp;

  blob.position.transform.translation.x = center_est.x;
  blob.position.transform.translation.y = center_est.y;
  blob.position.transform.translation.z = center_est.z;
  blob.position.transform.rotation.x = 0.;
  blob.position.transform.rotation.y = 0.;
  blob.position.transform.rotation.z = 0.;
  blob.position.transform.rotation.w = 1.;
  blob.position.header.stamp = leftImage_->header.stamp;


  blob.depth_density = depth_density;
  return blob;
}

void
HueBlob::checkInputsSynchronized()
{
  int threshold = 3 * all_received_;
  if (left_received_ > threshold
      || right_received_ > threshold || disp_received_ > threshold)
    {
      ROS_WARN
	("[hueblob] Low number of synchronized left/right/disparity triplets"
	 " received.\n"
	 "Left images received: %d\n"
	 "Right images received: %d\n"
	 "Disparity images received: %d\n"
	 "Synchronized triplets: %d\n"
	 "Possible issues:\n"
	 "\t* stereo_image_proc is not running.\n"
	 "\t* The cameras are not synchronized.\n"
	 "\t* The network is too slow. One or more images are dropped from each"
	 "triplet.",
	 left_received_, right_received_, disp_received_, all_received_);
    }
}
