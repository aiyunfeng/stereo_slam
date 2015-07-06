#include <ros/ros.h>

#include "frame.h"
#include "constants.h"
#include "tools.h"
#include "ldb.h"

using namespace tools;

namespace slam
{

  Frame::Frame() {}

  Frame::Frame(cv::Mat l_img, cv::Mat r_img, image_geometry::StereoCameraModel camera_model)
  {
    l_img.copyTo(l_img_);
    r_img.copyTo(r_img_);

    // Convert images to grayscale
    cv::Mat l_img_gray, r_img_gray;
    cv::cvtColor(l_img, l_img_gray, CV_RGB2GRAY);
    cv::cvtColor(r_img, r_img_gray, CV_RGB2GRAY);

    // Detect keypoints
    vector<cv::KeyPoint> l_kp, r_kp;
    cv::ORB orb(1000, 1.2f, 8, 14, 0, 2, 0, 14);
    orb(l_img_gray, noArray(), l_kp, noArray(), false);
    orb(r_img_gray, noArray(), r_kp, noArray(), false);

    // Extract descriptors
    cv::Mat l_desc, r_desc;
    LDB extractor_;
    extractor_.compute(l_img_gray, l_kp, l_desc, 0);
    extractor_.compute(r_img_gray, r_kp, r_desc, 0);

    // Left/right matching
    vector<cv::DMatch> matches, matches_filtered;
    Tools::ratioMatching(l_desc, r_desc, 0.8, matches);

    // Filter matches by epipolar
    for (size_t i=0; i<matches.size(); ++i)
    {
      if (abs(l_kp[matches[i].queryIdx].pt.y - r_kp[matches[i].trainIdx].pt.y) < 1.0)
        matches_filtered.push_back(matches[i]);
    }

    // Compute 3D points
    l_kp_.clear();
    r_kp_.clear();
    camera_points_.clear();
    l_desc_.release();
    r_desc_.release();
    for (size_t i=0; i<matches_filtered.size(); ++i)
    {
      cv::Point3d world_point;
      int l_idx = matches_filtered[i].queryIdx;
      int r_idx = matches_filtered[i].trainIdx;

      cv::Point2d l_point = l_kp[l_idx].pt;
      cv::Point2d r_point = r_kp[r_idx].pt;

      double disparity = l_point.x - r_point.x;
      camera_model.projectDisparityTo3d(l_point, disparity, world_point);

      // Save
      if ( isfinite(world_point.x) && isfinite(world_point.y) && isfinite(world_point.z) && world_point.z > 0)
      {
        l_kp_.push_back(l_kp[l_idx]);
        r_kp_.push_back(r_kp[r_idx]);
        l_desc_.push_back(l_desc.row(l_idx));
        r_desc_.push_back(r_desc.row(r_idx));
        camera_points_.push_back(world_point);
      }
    }
  }

  cv::Mat Frame::computeSift()
  {
    cv::Mat sift;
    if (l_img_.cols == 0)
      return sift;

    cv::initModule_nonfree();
    cv::Ptr<cv::DescriptorExtractor> cv_extractor;
    cv_extractor = cv::DescriptorExtractor::create("SIFT");
    cv_extractor->compute(l_img_, l_kp_, sift);

    return sift;
  }

  // FROM: http://codereview.stackexchange.com/questions/23966/density-based-clustering-of-image-keypoints
  void Frame::regionClustering()
  {
    clusters_.clear();
    vector< vector<int> > clusters;
    const float eps = 50.0;
    const int min_pts = 30;
    vector<bool> clustered;
    vector<int> noise;
    vector<bool> visited;
    vector<int> neighbor_pts;
    vector<int> neighbor_pts_;
    int c;

    uint no_keys = l_kp_.size();

    //init clustered and visited
    for(uint k=0; k<no_keys; k++)
    {
      clustered.push_back(false);
      visited.push_back(false);
    }

    c = -1;

    //for each unvisited point P in dataset keypoints
    for(uint i=0; i<no_keys; i++)
    {
      if(!visited[i])
      {
        // Mark P as visited
        visited[i] = true;
        neighbor_pts = regionQuery(&l_kp_, &l_kp_.at(i), eps);
        if(neighbor_pts.size() < min_pts)
        {
          // Mark P as Noise
          noise.push_back(i);
          clustered[i] = true;
        }
        else
        {
          clusters.push_back(vector<int>());
          c++;

          // expand cluster
          // add P to cluster c
          clusters[c].push_back(i);
          clustered[i] = true;

          // for each point P' in neighbor_pts
          for(uint j=0; j<neighbor_pts.size(); j++)
          {
            // if P' is not visited
            if(!visited[neighbor_pts[j]])
            {
              // Mark P' as visited
              visited[neighbor_pts[j]] = true;
              neighbor_pts_ = regionQuery(&l_kp_, &l_kp_.at(neighbor_pts[j]), eps);
              if(neighbor_pts_.size() >= min_pts)
              {
                neighbor_pts.insert(neighbor_pts.end(), neighbor_pts_.begin(), neighbor_pts_.end());
              }
            }
            // if P' is not yet a member of any cluster
            // add P' to cluster c
            if(!clustered[neighbor_pts[j]])
            {
              clusters[c].push_back(neighbor_pts[j]);
              clustered[neighbor_pts[j]] = true;
            }
          }
        }
      }
    }

    // Discard small clusters
    for (uint i=0; i<clusters.size(); i++)
    {
      if (clusters[i].size() >= min_pts)
        clusters_.push_back(clusters[i]);
      else
      {
        for (uint j=0; j<clusters[i].size(); j++)
          noise.push_back(clusters[i][j]);
      }
    }

    // Refine points treated as noise
    bool iterate = true;
    while (iterate && noise.size() > 0)
    {
      uint size_a = noise.size();
      vector<int> noise_tmp;
      for (uint n=0; n<noise.size(); n++)
      {
        int idx = -1;
        bool found = false;
        KeyPoint p_n = l_kp_.at(noise[n]);
        for (uint i=0; i<clusters_.size(); i++)
        {
          for (uint j=0; j<clusters_[i].size(); j++)
          {
            KeyPoint p_c = l_kp_.at(clusters_[i][j]);
            float dist = sqrt(pow((p_c.pt.x - p_n.pt.x),2)+pow((p_c.pt.y - p_n.pt.y),2));
            if(dist <= eps && dist != 0.0)
            {
              idx = i;
              found = true;
              break;
            }
          }
          if (found)
            break;
        }

        if (found && idx >= 0)
          clusters_[idx].push_back(noise[n]);
        else
          noise_tmp.push_back(noise[n]);
      }

      if (noise_tmp.size() == 0 || noise_tmp.size() == size_a)
        iterate = false;

      noise = noise_tmp;
    }

    // Treat noise as a cluster if enough points
    if (noise.size() >= min_pts)
      clusters_.push_back(noise);

    // Compute the clusters centroids
    for (uint i=0; i<clusters_.size(); i++)
    {
      Cloud::Ptr cluster_points(new Cloud);
      for (uint j=0; j<clusters_[i].size(); j++)
      {
        int idx = clusters_[i][j];
        cv::Point3f p_cv = camera_points_[idx];
        PointXYZ p(p_cv.x, p_cv.y, p_cv.z);
        cluster_points->push_back(p);
      }

      Eigen::Vector4f centroid;
      compute3DCentroid(*cluster_points, centroid);
      centroid = Tools::vector4fToIsometry(centroid, pose_);
      cluster_centroids_.push_back(centroid);
    }
  }

  vector<int> Frame::regionQuery(vector<cv::KeyPoint> *keypoints, cv::KeyPoint *keypoint, float eps)
  {
    float dist;
    vector<int> ret_keys;
    for(uint i=0; i<keypoints->size(); i++)
    {
      dist = sqrt(pow((keypoint->pt.x - keypoints->at(i).pt.x),2)+pow((keypoint->pt.y - keypoints->at(i).pt.y),2));
      if(dist <= eps && dist != 0.0)
      {
        ret_keys.push_back(i);
      }
    }
    return ret_keys;
  }

} //namespace slam