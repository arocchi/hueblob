#include <boost/foreach.hpp>
#include <boost/scope_exit.hpp>

#include <ros/ros.h>

#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>
#include <hueblob/Blob.h>
#include <hueblob/AddObject.h>
#include <cv.h>

#include "hueblob.h"

void nullDeleter(void*) {}

HueBlob::HueBlob()
  : nh_("hueblob"),
    it_(nh_),
    stereo_topic_prefix_ (),
    threshold_(),
    bridge_(),
    left_sub_(),
    right_sub_(),
    disparity_sub_(),
    sync_(3),
    blobs_pub_(nh_.advertise<hueblob::Blobs>("blobs", 5)),
    AddObject_srv_(nh_.advertiseService
		   ("add_objet", &HueBlob::AddObjectCallback, this)),
    TrackObject_srv_(nh_.advertiseService
		   ("track_objet", &HueBlob::TrackObjectCallback, this)),
    objects_()
{
  // Parameter initialization.
  ros::param::param<std::string>("stereo", stereo_topic_prefix_, "");
  ros::param::param<double>("threshold", threshold_, 75.);

  // Initialize the node subscribers, publishers and filters.
  setupInfrastructure(stereo_topic_prefix_);
}

HueBlob::~HueBlob()
{
  ROS_DEBUG("Destructing the node.");
}

void
HueBlob::spin()
{
  ros::Rate loop_rate(10);

  ROS_DEBUG("Entering the node main loop.");
  while (ros::ok())
    {
      hueblob::Blobs blobs;
      //FIXME: fill structure before publishing.
      blobs_pub_.publish(blobs);

      ros::spinOnce();
      loop_rate.sleep();
    }
}

void
HueBlob::setupInfrastructure(const std::string& stereo_prefix)
{
  stereo_topic_prefix_ = nh_.resolveName(stereo_prefix);

  const std::string left_topic =
    ros::names::clean(stereo_prefix + "/left/image_mono");
  const std::string right_topic =
    ros::names::clean(stereo_prefix + "/right/image_mono");
  const std::string disparity_topic =
    ros::names::clean(stereo_prefix + "/disparity");

  left_sub_.subscribe(it_, left_topic, 3);
  right_sub_.subscribe(it_, right_topic, 3);
  disparity_sub_.subscribe(nh_, disparity_topic, 3);

  //FIXME: is it needed to be reentrant?
  //sync_.disconnectAll();
  sync_.connectInput(left_sub_, right_sub_, disparity_sub_);
  sync_.registerCallback(boost::bind(&HueBlob::imageCallback,
				     this, _1, _2, _3));

  //FIXME: add callback checking that images are received.

  ROS_INFO("Subscribing to:\n\t* %s\n\t* %s\n\t* %s",
	   left_topic.c_str(), right_topic.c_str(),
	   disparity_topic.c_str());
}


#define CHECK_IMAGE_SIZE_(IMG)						\
  do {									\
    if (!IMG || width != IMG->width || height != IMG->height)		\
      IMG = cvCreateImage(cvSize(width, height), 8, 3);			\
  } while (0)

void
HueBlob::checkImagesSize(int width, int height)
{
  CHECK_IMAGE_SIZE_(trackImage);
  CHECK_IMAGE_SIZE_(hstrackImage[0]);
  CHECK_IMAGE_SIZE_(hstrackImage[1]);
  CHECK_IMAGE_SIZE_(trackBackProj);
  CHECK_IMAGE_SIZE_(thrBackProj);
}

#undef CHECK_IMAGE_SIZE_

void
HueBlob::imageCallback(const sensor_msgs::ImageConstPtr& left,
		       const sensor_msgs::ImageConstPtr& right,
		       const stereo_msgs::DisparityImageConstPtr& disparity_msg)
{
  // First, resize the images if required.
  checkImagesSize(left->width, left->height);

  //FIXME:
}

bool
HueBlob::AddObjectCallback(hueblob::AddObject::Request& request,
			   hueblob::AddObject::Response& response)
{
  CvHistogram** objHist;
  IplImage* gmodel;
  IplImage* mask;
  IplImage* hsv;
  IplImage* hs_planes[2];
  float max;
  int hist_size[] = {25, 25};
  // 0 (~0°red) to 180 (~360°red again)
  float hue_range[] = { 0, 250 };
  // 0 (black-gray-white) to 255 (pure spectrum color)
  float sat_range[] = { 0, 250 };
  float* hist_ranges[] = { hue_range, sat_range };
  IplImage* model;

  // Make sure all resources are released when exiting the method.
  BOOST_SCOPE_EXIT( (&hs_planes)(&hsv)(&mask)(&gmodel)(&model) )
    {
      cvReleaseImage(&hs_planes[1]);
      cvReleaseImage(&hs_planes[0]);
      cvReleaseImage(&hsv);
      cvReleaseImage(&mask);
      cvReleaseImage(&gmodel);
      cvReleaseImage(&model);
    } BOOST_SCOPE_EXIT_END

  // Convert ROS image to OpenCV.
  try
    {
      boost::shared_ptr<sensor_msgs::Image> image_ptr
	(&request.image, nullDeleter);
      model = bridge_.imgMsgToCv(image_ptr,"rgb8");
    }
  catch(const sensor_msgs::CvBridgeException& error)
    {
      ROS_ERROR("failed to convert image");
      return false;
    }

  // Get reference on the object.
  Object& object = objects_[request.name];

  // Emit a warning if the object already exists.
  if (object.anchor_x
      || object.anchor_y
      || object.anchor_z)
    ROS_WARN("Overwriting the object %s", request.name.c_str());

  // Initialize the object.
  object.anchor_x = request.anchor.x;
  object.anchor_y = request.anchor.y;
  object.anchor_z = request.anchor.z;

  ++object.nViews;

  //FIXME: use a C++ container instead.
  object.modelHistogram =
    (CvHistogram **)realloc(object.modelHistogram,
			    object.nViews * sizeof(*object.modelHistogram));
  objHist = &object.modelHistogram[object.nViews-1];

  // compute mask
  gmodel = cvCreateImage(cvGetSize(model), 8, 1);
  mask = cvCreateImage(cvGetSize(model), 8, 1);
  cvCvtColor(model, gmodel, CV_BGR2GRAY);
  cvThreshold(gmodel, mask, 5, 255, CV_THRESH_BINARY);

  // create histogram
  hsv = cvCreateImage(cvGetSize(model), 8, 3);
  hs_planes[0] = cvCreateImage(cvGetSize(model), 8, 1);
  hs_planes[1] = cvCreateImage(cvGetSize(model), 8, 1);

  cvCvtColor(model, hsv, CV_BGR2HSV);
  cvCvtPixToPlane(hsv, hs_planes[0], NULL, NULL, NULL);
  cvCvtPixToPlane(hsv, NULL, hs_planes[1], NULL, NULL);

  *objHist = cvCreateHist(2, hist_size, CV_HIST_ARRAY, hist_ranges, 1);

  // compute histogram
  cvCalcHist(hs_planes, *objHist, 0, mask);
  cvGetMinMaxHistValue(*objHist, 0, &max, 0, 0 );
  cvConvertScale((*objHist)->bins, (*objHist)->bins, max?255./max:0., 0);

  return true;
}

bool
HueBlob::TrackObjectCallback(hueblob::TrackObject::Request& request,
			     hueblob::TrackObject::Response& response)
{
  return true;
}
