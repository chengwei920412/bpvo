#include "utils/data_loader.h"
#include "utils/bounded_buffer.h"
#include "utils/program_options.h"
#include "utils/viz.h"

#include "bpvo/vo.h"
#include "bpvo/debug.h"
#include "bpvo/timer.h"
#include "bpvo/trajectory.h"
#include "bpvo/config.h"
#include "bpvo/utils.h"
#include "bpvo/point_cloud.h"

#include <iostream>
#include <fstream>

#include <opencv2/highgui/highgui.hpp>

using namespace bpvo;

void writePointCloud(std::string prefix, int i, const PointCloud& pc, float min_weight = 0.75f);

int main(int argc, char** argv)
{
  fprintf(stdout, "%s\n", BPVO_BUILD_STR);

  ProgramOptions options;
  options
      ("config,c", "/home/halismai/code/bpvo/conf/tsukuba.cfg", "config file")
      ("output,o", "output.txt", "trajectory output file")
      ("numframes,n", int(100), "number of frames to process")
      ("buffersize,b", int(16), "buffer size to load images")
      ("points,p", "", "store the points to files with the given prefix")
      ("dontshow,x", "do not show images")
      .parse(argc, argv);

  auto max_frames = options.get<int>("numframes");
  auto conf_fn = options.get<std::string>("config");
  auto do_show = !options.hasOption("dontshow");
  auto points_prefix = options.get<std::string>("points");

  auto data_loader = DataLoader::FromConfig(conf_fn);

  int buffer_size = options.get<int>("buffersize");
  typename DataLoaderThread::BufferType image_buffer(buffer_size);

  AlgorithmParameters params(conf_fn);
  std::cout << "------- AlgorithmParameters -------" << std::endl;
  std::cout << params << std::endl;
  std::cout << "-----------------------------------" << std::endl;

  auto vo = VisualOdometry(data_loader.get(), params);

  Trajectory trajectory;
  typename DataLoaderThread::BufferType::value_type frame;

  std::cout << data_loader->calibration() << std::endl;
  DataLoaderThread data_loader_thread(std::move(data_loader), image_buffer);

  double total_time = 0.0;
  int f_i = 0;
  while(f_i < max_frames) {
    if(image_buffer.pop(&frame)) {
      if(frame->image().empty()) {
        printf("\n");
        Warn("could not get data\n");
        break;
      }
      Timer timer;
      auto result = vo.addFrame(frame->image().ptr<uint8_t>(),
                                frame->disparity().ptr<float>());
      double tt = timer.stop().count();
      total_time += (tt / 1000.0);

      if(!points_prefix.empty())
      {
        //assert( result.pointCloud && "null point cloud?" );
        if(result.pointCloud)
          writePointCloud(points_prefix, f_i, *result.pointCloud);
      }

      f_i += 1;
      trajectory.push_back(result.pose);

      int num_iters = result.optimizerStatistics.front().numIterations;
      if(num_iters == params.maxIterations) {
        printf("\n");
        Warn("maximum iterations reached\n");
      }

      fprintf(stdout, "Frame %05d %*.2f ms @ %*.2f Hz %03d iters %20s num_points %-*d\r",
              f_i-1, 6, tt, 5, (f_i - 1) / total_time,  num_iters,
              ToString(result.keyFramingReason).c_str(), 8, vo.numPointsAtLevel(0));
      fflush(stdout);

      if(do_show) {
        cv::imshow("image", overlayDisparity(frame.get(), 0.5f));
        int k = 0xff & cv::waitKey(2);
        if(k == ' ') k = cv::waitKey(0);
        if(k == 'q' || k == 27)
          break;
      }
    }
  }

  fprintf(stdout, "\n");
  Info("Processed %d frames @ %0.2f Hz\n", f_i, f_i / total_time);

  {
    auto output_fn = options.get<std::string>("output");
    if(!output_fn.empty()) {
      Info("Writing trajectory to %s\n", output_fn.c_str());
      if(!trajectory.writeCameraPath(output_fn)) {
        Warn("failed to write trajectory to %s\n", output_fn.c_str());
      }
    }

    std::ofstream ofs("poses.txt");
    for(size_t i = 0;i < trajectory.size(); ++i) {
      ofs << trajectory[i] << std::endl;
    }
    ofs.close();
  }

  data_loader_thread.stop();
  while(data_loader_thread.isRunning())
    Sleep(10);

  Info("done\n");

  return 0;
}


void writePointCloud(std::string prefix, int i, const PointCloud& pc, float min_weight)
{
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s_%05d.txt", prefix.c_str(), i);

  PointCloud pc_out;
  pc_out.reserve( pc.size() );

  std::cout << "POINT CLOUD POSE\n\n" << pc.pose() << "\n\n\n";

  FILE* fp = fopen(buf, "w");
  THROW_ERROR_IF(fp == NULL, "failed to open file");

  for(size_t i = 0; i < pc.size(); ++i)
  {
    if(pc[i].weight() > min_weight) {
      auto p = pc[i];
      fprintf(fp, "%f %f %f\n", p.xyzw()[0], p.xyzw()[1], p.xyzw()[2]);

      p.xyzw() = pc.pose() * p.xyzw();
      pc_out.push_back(p);
    }
  }

  fclose(fp);

  snprintf(buf, sizeof(buf), "%s_%05d.ply", prefix.c_str(), i);

  ToPlyFile(std::string(buf), pc_out);
}

