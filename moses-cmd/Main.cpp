// $Id: MainMT.cpp 3045 2010-04-05 13:07:29Z hieuhoang1972 $

/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2009 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

/**
 * Moses main, for single-threaded and multi-threaded.
 **/
#include <exception>
#include <fstream>
#include <sstream>
#include <vector>

#include "util/usage.hh"

#ifdef WIN32
// Include Visual Leak Detector
//#include <vld.h>
#endif

#include "moses/IOWrapper.h"
#include "moses/Hypothesis.h"
#include "moses/HypergraphOutput.h"
#include "moses/Manager.h"
#include "moses/StaticData.h"
#include "moses/TypeDef.h"
#include "moses/Util.h"
#include "moses/Timer.h"
#include "moses/TranslationModel/PhraseDictionary.h"
#include "moses/FF/StatefulFeatureFunction.h"
#include "moses/FF/StatelessFeatureFunction.h"
#include "moses/TranslationTask.h"

#ifdef HAVE_PROTOBUF
#include "hypergraph.pb.h"
#endif

using namespace std;
using namespace Moses;
using namespace MosesCmd;

namespace MosesCmd
{

void OutputFeatureWeightsForHypergraph(std::ostream &outputSearchGraphStream)
{
  outputSearchGraphStream.setf(std::ios::fixed);
  outputSearchGraphStream.precision(6);
  StaticData::Instance().GetAllWeights().Save(outputSearchGraphStream);
}


} //namespace

/** main function of the command line version of the decoder **/
int main(int argc, char** argv)
{
  try {

#ifdef HAVE_PROTOBUF
    GOOGLE_PROTOBUF_VERIFY_VERSION;
#endif
    
    // echo command line, if verbose
    IFVERBOSE(1) {
      TRACE_ERR("command: ");
      for(int i=0; i<argc; ++i) TRACE_ERR(argv[i]<<" ");
      TRACE_ERR(endl);
    }

    // set number of significant decimals in output
    fix(cout,PRECISION);
    fix(cerr,PRECISION);

    // load all the settings into the Parameter class
    // (stores them as strings, or array of strings)
    Parameter params;
    if (!params.LoadParam(argc,argv)) {
      exit(1);
    }


    // initialize all "global" variables, which are stored in StaticData
    // note: this also loads models such as the language model, etc.
    if (!StaticData::LoadDataStatic(&params, argv[0])) {
      exit(1);
    }

    // setting "-show-weights" -> just dump out weights and exit
    if (params.isParamSpecified("show-weights")) {
      ShowWeights();
      exit(0);
    }

    // shorthand for accessing information in StaticData
    const StaticData& staticData = StaticData::Instance();


    //initialise random numbers
    srand(time(NULL));

    // set up read/writing class
    IOWrapper* ioWrapper = GetIOWrapper(staticData);
    if (!ioWrapper) {
      cerr << "Error; Failed to create IO object" << endl;
      exit(1);
    }

    // check on weights
    const ScoreComponentCollection& weights = staticData.GetAllWeights();
    IFVERBOSE(2) {
      TRACE_ERR("The global weight vector looks like this: ");
      TRACE_ERR(weights);
      TRACE_ERR("\n");
    }
    boost::shared_ptr<HypergraphOutput<Manager> > hypergraphOutput; 
    if (staticData.GetOutputSearchGraphHypergraph()) {
      hypergraphOutput.reset(new HypergraphOutput<Manager>(PRECISION));
    }


    // initialize output streams
    // note: we can't just write to STDOUT or files
    // because multithreading may return sentences in shuffled order
    auto_ptr<OutputCollector> latticeSamplesCollector; //for lattice samples
    auto_ptr<ofstream> nbestOut;
    auto_ptr<ofstream> latticeSamplesOut;
    size_t nbestSize = staticData.GetNBestSize();
    string nbestFile = staticData.GetNBestFilePath();
    bool output1best = true;
    if (nbestSize) {
      if (nbestFile == "-" || nbestFile == "/dev/stdout") {
        // nbest to stdout, no 1-best
        output1best = false;
      } else {
        // nbest to file, 1-best to stdout
        nbestOut.reset(new ofstream(nbestFile.c_str()));
        if (!nbestOut->good()) {
          TRACE_ERR("ERROR: Failed to open " << nbestFile << " for nbest lists" << endl);
          exit(1);
        }
      }
    }
    size_t latticeSamplesSize = staticData.GetLatticeSamplesSize();
    string latticeSamplesFile = staticData.GetLatticeSamplesFilePath();
    if (latticeSamplesSize) {
      if (latticeSamplesFile == "-" || latticeSamplesFile == "/dev/stdout") {
        latticeSamplesCollector.reset(new OutputCollector());
        output1best = false;
      } else {
        latticeSamplesOut.reset(new ofstream(latticeSamplesFile.c_str()));
        if (!latticeSamplesOut->good()) {
          TRACE_ERR("ERROR: Failed to open " << latticeSamplesFile << " for lattice samples" << endl);
          exit(1);
        }
        latticeSamplesCollector.reset(new OutputCollector(latticeSamplesOut.get()));
      }
    }

#ifdef WITH_THREADS
    ThreadPool pool(staticData.ThreadCount());
#endif

    // main loop over set of input sentences
    InputType* source = NULL;
    size_t lineCount = staticData.GetStartTranslationId();
    while(ReadInput(*ioWrapper,staticData.GetInputType(),source)) {
      IFVERBOSE(1) {
        ResetUserTime();
      }

      FeatureFunction::CallChangeSource(source);

      // set up task of translating one sentence
      TranslationTask* task =
        new TranslationTask(lineCount,source, *ioWrapper,
                            latticeSamplesCollector.get(),
                            staticData.GetOutputSearchGraphSLF(),
                            hypergraphOutput);
      // execute task
#ifdef WITH_THREADS
      pool.Submit(task);
#else
      task->Run();
      delete task;
#endif

      source = NULL; //make sure it doesn't get deleted
      ++lineCount;
    }

    // we are done, finishing up
#ifdef WITH_THREADS
    pool.Stop(true); //flush remaining jobs
#endif

    delete ioWrapper;
    FeatureFunction::Destroy();

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  IFVERBOSE(1) util::PrintUsage(std::cerr);

#ifndef EXIT_RETURN
  //This avoids that destructors are called (it can take a long time)
  exit(EXIT_SUCCESS);
#else
  return EXIT_SUCCESS;
#endif
}
