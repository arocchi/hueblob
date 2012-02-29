#include <ros/ros.h>
#include <ros/console.h>

// Image transport.
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>


// Message filters.
# include <message_filters/subscriber.h>
# include <message_filters/sync_policies/exact_time.h>
# include <message_filters/sync_policies/approximate_time.h>
# include <message_filters/synchronizer.h>

// Msgs
#include <stereo_msgs/DisparityImage.h>
#include <sensor_msgs/CameraInfo.h>
#include <hueblob/RoiStamped.h>
#include <sensor_msgs/image_encodings.h>
#include <visualization_msgs/Marker.h>
#include <hueblob/Blob.h>

#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/features/feature.h>
#include "pcl/filters/statistical_outlier_removal.h"

#include "cv.h"
#include "highgui.h"

#include <cv_bridge/cv_bridge.h>
#include <Eigen/Dense>

// tf
#include <tf/transform_broadcaster.h>

namespace
{

  inline void projectTo3d(float u, float v, float disparity,
                          const stereo_msgs::DisparityImage &disparity_image,
                          const sensor_msgs::CameraInfo &camera_info,
                          float &x, float &y, float &z
                          )
  {

    float fx = camera_info.P[0*4+0];
    float fy = camera_info.P[1*4+1];
    float cx = camera_info.P[0*4+2];
    float cy = camera_info.P[1*4+2];
    // float Tx = camera_info.P[0*4+3];
    // float Ty = camera_info.P[1*4+3];

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
    if (h >= disparity_image.image.height || w >= disparity_image.image.width)
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
                  const sensor_msgs::Image &bgr_image,
                  const sensor_msgs::Image &mono_image,
                  const hueblob::RoiStamped & roi_stamped,
                  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_raw,
                  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_filtered
                  )
  {
    namespace enc = sensor_msgs::image_encodings;
    cv_bridge::CvImagePtr cv_rgb_ptr  = cv_bridge::toCvCopy(bgr_image, enc::BGR8);
    //cv_bridge::CvImagePtr cv_mono_ptr = cv_bridge::toCvCopy(mono_image, enc::MONO8);

    const sensor_msgs::RegionOfInterest roi = roi_stamped.roi;
    for (unsigned i = roi.y_offset; i < roi.y_offset + roi.height; ++i)
      for (unsigned j = roi.x_offset; j < roi.x_offset + roi.width; ++j)
        {
          if (!hasDisparityValue(disparity_image, i, j))
            continue;
          float disparity, x, y, z;
          unsigned u = i - roi.y_offset;
          unsigned v = j - roi.x_offset;
          getPoint(disparity_image.image, i, j, disparity);
          ROS_ASSERT(disparity_image.max_disparity != 0.0);
          if (disparity == 0)
            continue;


          unsigned char mono;
          memcpy(&mono, &(mono_image.data.at( u*mono_image.step + sizeof(char)*v )),
                 sizeof(char));
          //ROS_INFO_STREAM("Mono "<< u << " " << v << " " << (int) mono);
          if (mono == 0 and !cloud_raw)
            continue;

          projectTo3d(j, i, disparity,  disparity_image,
                      camera_info, x, y, z);
          pcl::PointXYZRGB p;
          p.x = x;
          p.y = y;
          p.z = z;
          cv::Vec3b rgb = cv_rgb_ptr->image.at<cv::Vec3b>(u, v);
          p.r = rgb[2];
          p.g = rgb[1];
          p.b = rgb[0];
          //ROS_INFO_STREAM(rgb[0]);
          cloud_raw->points.push_back(p);
          cloud_raw->header = roi_stamped.header;

          if (mono)
            {
              cloud_filtered->points.push_back(p);
              cloud_filtered->header = roi_stamped.header;
            }
        }

    static pcl::StatisticalOutlierRemoval<pcl::PointXYZRGB> sor;
    sor.setMeanK (50);
    sor.setStddevMulThresh (1.0);

    if (cloud_raw)
      {
        sor.setInputCloud (cloud_raw);
        sor.filter (*cloud_raw);
      }
    sor.setInputCloud (cloud_filtered);
    sor.filter (*cloud_filtered);
  }

} // end of anonymous namespace.




using namespace std;
class Projector
{
public:
  Projector();
  virtual ~Projector(){};

private:
  void callback(const sensor_msgs::CameraInfoConstPtr& info,
                const sensor_msgs::ImageConstPtr& bgr_image,
                const sensor_msgs::ImageConstPtr& mono_image,
                const stereo_msgs::DisparityImageConstPtr& disparity,
                const hueblob::RoiStampedConstPtr& box
                );

  typedef message_filters::sync_policies::ApproximateTime< sensor_msgs::CameraInfo,
                                                           sensor_msgs::Image,
                                                           sensor_msgs::Image,
                                                           stereo_msgs::DisparityImage,
                                                           hueblob::RoiStamped
                                                           > ApproximatePolicy;
  typedef message_filters::Synchronizer<ApproximatePolicy> ApproximateSync;


  ros::NodeHandle nh_;
  image_transport::ImageTransport it_;
  ApproximateSync sync_;
  message_filters::Subscriber<hueblob::RoiStamped> roi_sub_;
  message_filters::Subscriber<sensor_msgs::CameraInfo> camera_info_sub_;
  message_filters::Subscriber<stereo_msgs::DisparityImage> disparity_sub_;
  image_transport::SubscriberFilter bgr_image_sub_, mono_image_sub_;
  ros::Publisher cloud_pub_, cloud_filtered_pub_, marker_pub_, blob3d_pub_;


  tf::TransformBroadcaster br_;
  string base_name_;
};


Projector::Projector()
  : nh_("blob2CloudProjector"),
    it_(nh_),
    sync_(50),
    roi_sub_(),
    camera_info_sub_(),
    disparity_sub_(),
    br_()
{
  string blob2d_topic, blob3d_topic, disparity_topic, cloud_topic, blob_name, \
    camera_info_topic, bgr_image_topic, mono_image_topic, \
    cloud_filtered_topic, marker_topic;
  ros::param::param<string>("~blob2d", blob2d_topic, "blobs/rose/blob2d");
  ros::param::param<string>("~blob3d", blob3d_topic, "blobs/rose/blob3d");
  ros::param::param<string>("~disparity", disparity_topic, "disparity");
  ros::param::param<string>("~camera_info", camera_info_topic, "left/camera_info");
  ros::param::param<string>("~bgr_image", bgr_image_topic, "blobs/rose/bgr_image");
  ros::param::param<string>("~mono_image", mono_image_topic, "blobs/rose/mono_image");
  ros::param::param<string>("~blob_name", blob_name, "rose");


  blob2d_topic         = ros::names::resolve(blob2d_topic);
  blob3d_topic         = ros::names::resolve(blob3d_topic);
  disparity_topic      = ros::names::resolve(disparity_topic);
  cloud_topic          = ros::names::resolve(cloud_topic);
  camera_info_topic    = ros::names::resolve(camera_info_topic);
  bgr_image_topic      = ros::names::resolve(bgr_image_topic);
  mono_image_topic     = ros::names::resolve(mono_image_topic);
  base_name_           = ros::names::resolve(ros::names::append("blobs" , blob_name));
  cloud_filtered_topic = ros::names::append(base_name_, "points/filtered");
  marker_topic         = ros::names::append(base_name_, "points/marker");
  cloud_topic          = ros::names::append(base_name_, "points/raw");

  cloud_filtered_pub_  = nh_.advertise<pcl::PointCloud<pcl::PointXYZRGB> > (cloud_filtered_topic, 1);
  marker_pub_ = nh_.advertise<visualization_msgs::Marker>  (marker_topic, 1);
  cloud_pub_  = nh_.advertise<pcl::PointCloud<pcl::PointXYZRGB> > (cloud_topic, 1);
  blob3d_pub_  = nh_.advertise<hueblob::Blob> (blob3d_topic, 1);

  roi_sub_.subscribe(nh_, blob2d_topic, 10);
  camera_info_sub_.subscribe(nh_, camera_info_topic, 10);
  bgr_image_sub_.subscribe(it_, bgr_image_topic, 10);
  mono_image_sub_.subscribe(it_, mono_image_topic, 10);
  disparity_sub_.subscribe(nh_, disparity_topic, 10);

  sync_.connectInput(camera_info_sub_, bgr_image_sub_, mono_image_sub_, disparity_sub_, roi_sub_);
  sync_.registerCallback(boost::bind(&Projector::callback,
                                     this, _1, _2, _3, _4, _5));

  ROS_INFO_STREAM(endl<< "Listening to:"
                  << "\n\t* " << blob2d_topic
                  << "\n\t* " << disparity_topic
                  << "\n\t* " << camera_info_topic
                  << "\n\t* " << bgr_image_topic
                  << "\n\t* " << mono_image_topic
                  << endl
                  << "Publishing to:"
                  << "\n\t* " << cloud_topic
		  << "\n\t* " << cloud_filtered_topic
		  );

}

void Projector::callback(const sensor_msgs::CameraInfoConstPtr& info,
                         const sensor_msgs::ImageConstPtr& bgr_image,
                         const sensor_msgs::ImageConstPtr& mono_image,
                         const stereo_msgs::DisparityImageConstPtr& disparity,
                         const hueblob::RoiStampedConstPtr& roi_stamped
                )
{

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_raw(new pcl::PointCloud<pcl::PointXYZRGB>);
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_filtered (new pcl::PointCloud<pcl::PointXYZRGB>);
  // ROS_INFO_STREAM(roi_stamped->roi.x_offset << " " << roi_stamped->roi.y_offset << " "
  //                 << roi_stamped->roi.width << " " << roi_stamped->roi.height);

  get3dCloud(*disparity, *info, *bgr_image, *mono_image,
             *roi_stamped,
             cloud_raw, cloud_filtered);

  cloud_pub_.publish(cloud_raw);
  cloud_filtered_pub_.publish(cloud_filtered);
  Eigen::Vector4f centroid (0., 0., 0., 0.);
  pcl::compute3DCentroid(*cloud_filtered, centroid);
  visualization_msgs::Marker marker;
  marker.header = roi_stamped->header;
  marker.type = visualization_msgs::Marker::SPHERE;
  marker.pose.position.x = centroid[0];
  marker.pose.position.y = centroid[1];
  marker.pose.position.z = centroid[2];
  marker.scale.x = 0.1;
  marker.scale.y = 0.1;
  marker.scale.z = 0.1;
  marker_pub_.publish(marker);
  tf::Transform transform;
  transform.setIdentity();
  transform.setOrigin(tf::Vector3(centroid[0],centroid[1],centroid[2]));
  br_.sendTransform(tf::StampedTransform(transform, roi_stamped->header.stamp,
                                         roi_stamped->header.frame_id,
                                         ros::names::append(base_name_, "centroid")
                                         )
                    );
  hueblob::Blob blob;
  blob.cloud_centroid.transform.translation.x = centroid[0];
  blob.cloud_centroid.transform.translation.y = centroid[1];
  blob.cloud_centroid.transform.translation.z = centroid[2];
  blob.cloud_centroid.transform.rotation.x = 0.;
  blob.cloud_centroid.transform.rotation.y = 0.;
  blob.cloud_centroid.transform.rotation.z = 0.;
  blob.cloud_centroid.transform.rotation.w = 1.;
  blob.cloud_centroid.header.stamp = roi_stamped->header.stamp;
  blob.depth_density = 1.*cloud_filtered->points.size()/(roi_stamped->roi.width*
							 roi_stamped->roi.height);
  blob.boundingbox_2d.resize(4);
  blob.boundingbox_2d[0] = roi_stamped->roi.x_offset;
  blob.boundingbox_2d[1] = roi_stamped->roi.y_offset;
  blob.boundingbox_2d[2] = roi_stamped->roi.width;
  blob.boundingbox_2d[3] = roi_stamped->roi.height;

  blob.header = roi_stamped->header;
  blob3d_pub_.publish(blob);
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "blob2CloudProjector");
  Projector p;
  ros::spin();
}
