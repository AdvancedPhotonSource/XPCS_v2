/**

Copyright (c) 2016, UChicago Argonne, LLC. All rights reserved.

Copyright 2016. UChicago Argonne, LLC. This software was produced 
under U.S. Government contract DE-AC02-06CH11357 for Argonne National 
Laboratory (ANL), which is operated by UChicago Argonne, LLC for the 
U.S. Department of Energy. The U.S. Government has rights to use, 
reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR 
UChicago Argonne, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR a
ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is 
modified to produce derivative works, such modified software should 
be clearly marked, so as not to confuse it with the version available 
from ANL.

Additionally, redistribution and use in source and binary forms, with 
or without modification, are permitted provided that the following 
conditions are met:

    * Redistributions of source code must retain the above copyright 
      notice, this list of conditions and the following disclaimer. 

    * Redistributions in binary form must reproduce the above copyright 
      notice, this list of conditions and the following disclaimer in 
      the documentation and/or other materials provided with the 
      distribution. 

    * Neither the name of UChicago Argonne, LLC, Argonne National 
      Laboratory, ANL, the U.S. Government, nor the names of its 
      contributors may be used to endorse or promote products derived 
      from this software without specific prior written permission. 

THIS SOFTWARE IS PROVIDED BY UChicago Argonne, LLC AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL UChicago 
Argonne, LLC OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN 
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
POSSIBILITY OF SUCH DAMAGE.

**/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <memory>
#include <map>
#include <vector>
#include <iostream>

#include <sys/stat.h>

#include "hdf5.h"
#include "gflags/gflags.h"
#include "spdlog/spdlog.h"

#include "corr.h"
#include "xpcs/configuration.h"
#include "h5_result.h"
#include "benchmark.h"
#include "xpcs/io/reader.h"
#include "xpcs/io/imm.h"
#include "xpcs/io/ufxc.h"
#include "xpcs/io/rigaku.h"
#include "xpcs/filter/filter.h"
#include "xpcs/filter/burst_filter.h"
#include "xpcs/filter/sparse_filter.h"
#include "xpcs/filter/sparse_filter_burst.h"
#include "xpcs/filter/dense_filter.h"
#include "xpcs/filter/stride.h"
#include "xpcs/data_structure/dark_image.h"
#include "xpcs/data_structure/sparse_data.h"
#include "xpcs/data_structure/row.h"

using namespace std;
namespace spd = spdlog; 

DEFINE_bool(g2out, false, "Write intermediate output from G2 computation");
DEFINE_bool(darkout, false, "Write dark average and std-data");
DEFINE_bool(ufxc, false, "IF the file format is from ufxc photon counting detector.");
DEFINE_bool(rigaku, false, "IF the file format is from rigaku photon counting detector.");
DEFINE_int32(frameout, false, "Number of post-processed frames to write out for debuggin.");
DEFINE_string(imm, "", "The path to IMM file. By default the file specified in HDF5 metadata is used");
DEFINE_string(inpath, "", "The path prefix to replace");
DEFINE_string(outpath, "", "The path prefix to replace with");
DEFINE_string(entry, "", "The metadata path in HDF5 file");

void compute_burst(xpcs::Configuration *conf)
{
  int frames = conf->getFrameTodoCount();

  int pixels = conf->getFrameWidth() * conf->getFrameHeight();
  int maxLevel = xpcs::Corr::calculateLevelMax(frames, conf->DelaysPerLevel());
  vector<std::tuple<int,int> > delays_per_level = xpcs::Corr::delaysPerLevel(frames, conf->DelaysPerLevel(), maxLevel);

  float* g2s = new float[pixels * delays_per_level.size()];
  float* ips = new float[pixels * delays_per_level.size()];
  float* ifs = new float[pixels * delays_per_level.size()];

  float* g2s_average = new float[pixels * delays_per_level.size()];
  float* ips_average = new float[pixels * delays_per_level.size()];
  float* ifs_average = new float[pixels * delays_per_level.size()];

  xpcs::io::Reader *reader = NULL; 

  if (FLAGS_ufxc) {
    printf("Loading UFXC as binary\n");
    reader = new xpcs::io::Ufxc(conf->getIMMFilePath().c_str());
  } else if (FLAGS_rigaku) {
    printf("Loading Rigaku as binary\n");
    reader = new xpcs::io::Rigaku(conf->getIMMFilePath().c_str());
  } else {
    reader = new xpcs::io::Imm(conf->getIMMFilePath().c_str());
  }

  xpcs::filter::BurstFilter *filter = new xpcs::filter::SparseFilterBurst();

  int burst_size = conf->NumberOfBursts();
  int total_bursts = frames / burst_size;

  printf("Total bursts = %d\n", total_bursts);

  // The last frame outside the stride will be ignored. 
  int f = 0;
  while (f < total_bursts) {

    struct xpcs::io::ImmBlock* data = reader->NextFrames(burst_size);
    filter->Apply(data);
    
    for (int i = 0; i < (pixels * delays_per_level.size()); i++) {
      g2s[i] = 0.0f;
      ips[i] = 0.0f;
      ifs[i] = 0.0f;
    }
    
    xpcs::Corr::multiTau2(filter->BurstData(), g2s, ips, ifs);

    for (int i = 0; i < (pixels * delays_per_level.size()); i++) {
      g2s_average[i] += g2s[i];
      ips_average[i] += ips[i];
      ifs_average[i] += ifs[i];
    }

    f++;
  }

  for (int i = 0; i < (pixels * delays_per_level.size()); i++) {
    g2s_average[i] /= total_bursts;
    ips_average[i] /= total_bursts;
    ifs_average[i] /= total_bursts;
  }

  // xpcs::Benchmark benchmark("Normalizing Data");
  Eigen::MatrixXf G2s = Eigen::Map<Eigen::MatrixXf>(g2s_average, pixels, delays_per_level.size());
  Eigen::MatrixXf IPs = Eigen::Map<Eigen::MatrixXf>(ips_average, pixels, delays_per_level.size());
  Eigen::MatrixXf IFs = Eigen::Map<Eigen::MatrixXf>(ifs_average, pixels, delays_per_level.size());

  xpcs::H5Result::write2DData(conf->getFilename(), conf->OutputPath(), "G2_burst", G2s);
  xpcs::H5Result::write2DData(conf->getFilename(), conf->OutputPath(), "IP_burst", IPs);
  xpcs::H5Result::write2DData(conf->getFilename(), conf->OutputPath(), "IF_burst", IFs);

  for (int i = 0; i < (pixels * delays_per_level.size()); i++) {
    g2s[i] = 0.0f;
    ips[i] = 0.0f;
    ifs[i] = 0.0f;
  }

  xpcs::Corr::multiTau2(filter->Data(), g2s, ips, ifs);

  Eigen::MatrixXf G2s2 = Eigen::Map<Eigen::MatrixXf>(g2s, pixels, delays_per_level.size());
  Eigen::MatrixXf IPs2 = Eigen::Map<Eigen::MatrixXf>(ips, pixels, delays_per_level.size());
  Eigen::MatrixXf IFs2 = Eigen::Map<Eigen::MatrixXf>(ifs, pixels, delays_per_level.size());

  xpcs::H5Result::write2DData(conf->getFilename(), conf->OutputPath(), "G2", G2s2);
  xpcs::H5Result::write2DData(conf->getFilename(), conf->OutputPath(), "IP", IPs2);
  xpcs::H5Result::write2DData(conf->getFilename(), conf->OutputPath(), "IF", IFs2);

}

int main(int argc, char** argv)
{
  if (argc < 2) {
      fprintf(stderr, "Please specify a HDF5 metadata file\n");
      return 1;
  }

  xpcs::Benchmark total("Total");
 
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  auto console = spd::stdout_color_mt("console");

  std::string entry = "/xpcs";

  if (!FLAGS_entry.empty())
      entry = FLAGS_entry;

  console->info("H5 metadata path {}", entry.c_str());

  xpcs::Configuration *conf = xpcs::Configuration::instance();
  conf->init(argv[1], entry);

  if (!FLAGS_imm.empty())
      conf->setIMMFilePath(FLAGS_imm);

  if (!FLAGS_inpath.empty() and !FLAGS_outpath.empty())
  {
      std::string file = conf->getIMMFilePath();
      std::string::size_type pos = file.find(FLAGS_inpath);

      if (pos != std::string::npos)
      {
          file.replace(file.begin()+pos,
                       file.end()-(strlen(file.c_str()) - strlen(FLAGS_inpath.c_str())),
                       FLAGS_outpath.begin(), FLAGS_outpath.end());
      }

      conf->setIMMFilePath(file);
  }

  console->info("Processing IMM file at path {}..", conf->getIMMFilePath().c_str());
  struct stat st;
  if(stat(conf->getIMMFilePath().c_str(), &st) == 0) {
    char prefix[] = {' ', 'K', 'M', 'G', 'T'};
    unsigned long size = st.st_size;
    int suffix = 0;
    while (size >= 1024) {
       size = size / 1024;
       suffix++;
    }

    console->info("File size {0} {1}bytes", suffix > 0 ? (float)st.st_size/ pow(1024.0, suffix) : st.st_size, prefix[suffix]);
  }

  int* dqmap = conf->getDQMap();
  int *sqmap = conf->getSQMap();

  int frames = conf->getFrameTodoCount();
  int frameFrom = conf->getFrameStartTodo();
  int frameTo = frameFrom + frames; //conf->getFrameEndTodo();
  int swindow = conf->getStaticWindowSize();
  int stride_factor = conf->FrameStride();
  int average_factor = conf->FrameAverage();
  int bursts = conf->NumberOfBursts();

  console->info("Data frames={0} stride={1} average={2}", frames, stride_factor, average_factor);
  console->debug("Frames count={0}, from={1}, todo={2}", frames, frameFrom, frameTo);
  console->debug("Brust count={0}", bursts);

  compute_burst(conf);

}