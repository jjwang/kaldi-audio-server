// audio-server-online2-nnet3.cc

// Copyright 2016-2017  Junjie Wang, Yanqing Sun

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include <signal.h>
#include <mcheck.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctime>

#include "feat/wave-reader.h"
#include "online2/online-nnet2-feature-pipeline.h"
#include "online2/online-nnet3-decoding.h"
#include "online2/onlinebin-util.h"
#include "online2/online-timing.h"
#include "online2/online-endpoint.h"
#include "online/online-tcp-source.h"
#include "fstext/fstext-lib.h"
#include "lat/lattice-functions.h"
#include "lat/kaldi-lattice.h"
#include "lat/word-align-lattice-lexicon.h"
#include "util/kaldi-thread.h"
#include "nnet3/nnet-utils.h"

int32 packet_size = 512;
kaldi::BaseFloat chunk_length_secs = 0.18;
kaldi::BaseFloat secs_per_frame = 0.01;
kaldi::nnet3::NnetSimpleLoopedComputationOptions decodable_opts;

namespace kaldi {

#pragma message("To support CMVN in feature pipeline, you must add virtual " \
  "declaration to OnlineNnet2FeaturePipeline::InputFeature() function in " \
  "online2/online-nnet2-feature-pipeline.h")

struct OnlineCmvnNnet2FeaturePipelineConfig :
    public OnlineNnet2FeaturePipelineConfig {
  std::string cmvn_config;
  std::string global_cmvn_stats_rxfilename;

  OnlineCmvnNnet2FeaturePipelineConfig() { }

  void Register(OptionsItf *opts) {
    opts->Register("cmvn-config", &cmvn_config, "Configuration class "
                   "file for online CMVN features (e.g. online_cmvn.conf)");
    opts->Register("global-cmvn-stats", &global_cmvn_stats_rxfilename,
                   "(Extended) filename for global CMVN stats, e.g. obtained "
                   "from 'matrix-sum scp:data/train/cmvn.scp -'");
    OnlineNnet2FeaturePipelineConfig::Register(opts);
  }
};

struct OnlineCmvnNnet2FeaturePipelineInfo :
    public OnlineNnet2FeaturePipelineInfo {
  OnlineCmvnNnet2FeaturePipelineConfig config_;

  OnlineCmvnNnet2FeaturePipelineInfo(
      const OnlineCmvnNnet2FeaturePipelineConfig &config) :
      OnlineNnet2FeaturePipelineInfo(config) {
    config_ = config;
  }
};

class OnlineCmvnNnet2FeaturePipeline: public OnlineNnet2FeaturePipeline {
  Matrix<BaseFloat> global_cmvn_stats_;  // Global CMVN stats.
  OnlineCmvnOptions cmvn_opts;  // Options for online CMN/CMVN computation.
  OnlineCmvn *cmvn_;

 public:
  OnlineCmvnNnet2FeaturePipeline(
      const OnlineCmvnNnet2FeaturePipelineInfo &info) :
      OnlineNnet2FeaturePipeline(info) {
    if (info.config_.global_cmvn_stats_rxfilename != "") {
      ReadKaldiObject(info.config_.global_cmvn_stats_rxfilename,
                      &global_cmvn_stats_);
    }

    if (info.config_.cmvn_config != "") {
      ReadConfigFromFile(info.config_.cmvn_config, &cmvn_opts);
      KALDI_ASSERT(global_cmvn_stats_.NumRows() != 0);
      if (info.add_pitch || info.use_ivectors) {
        KALDI_ERR << "CMVN does not support pitch and ivector.";
      }
      Matrix<double> global_cmvn_stats_dbl(global_cmvn_stats_);
      OnlineCmvnState initial_state(global_cmvn_stats_dbl);
      cmvn_ = new OnlineCmvn(cmvn_opts,
                             initial_state,
                             OnlineNnet2FeaturePipeline::InputFeature());
      KALDI_VLOG(1) << "CMVN is enabled for feature pipeline";
    } else {
      cmvn_ = NULL;
    }
  }

  virtual OnlineFeatureInterface *InputFeature() {
    return (cmvn_ != NULL) ? cmvn_ : OnlineNnet2FeaturePipeline::InputFeature();
  }
};

/*
 * This class is for a very simple TCP server implementation
 * in UNIX sockets.
 */
class TcpServer {
 public:
  TcpServer();
  ~TcpServer();

  bool Listen(int32 port);  // start listening on a given port
  int32 Accept();  // accept a client and return its descriptor

 private:
  struct sockaddr_in _h_addr_;
  int32 _server_desc_;
};

class DecoderPool {
 public:
  DecoderPool();
  ~DecoderPool();

  // Decoder related data structures
  LatticeFasterDecoderConfig _config;
  TransitionModel _tmodel;
  nnet3::AmNnetSimple _am_nnet;
  fst::Fst<fst::StdArc> *_fst;
  OnlineCmvnNnet2FeaturePipelineInfo *_feature_info;
  fst::SymbolTable *_word_syms;
  WordAlignLatticeLexiconInfo *_lexicon_info;

  struct DecoderThread {
    DecoderPool *_pool;
    pthread_t _tid;

    // Variables for network programming
    int32 _client_socket;
    pthread_mutex_t _lock;
    bool _is_free;
  };
  static void* ThreadProc(void* para);
  void Run(const int32 &n);
  void NewTask(int32 client_socket);
  bool IsBusy();

 private:
  DecoderThread* _decoder_threads;
  int32 _num;
};

bool WriteLine(int32 socket, std::string line) {
  line = line + "\n";

  const char* p = line.c_str();
  int32 to_write = line.size();
  int32 wrote = 0;
  while (to_write > 0) {
    int32 ret = write(socket, p + wrote, to_write);
    if (ret <= 0)
      return false;

    to_write -= ret;
    wrote += ret;
  }

  return true;
}

void GetDiagnosticsAndPrintOutput(
    int32 socket,
    bool end_of_utterance,
    int32 start_time,
    const std::string &utt,
    const TransitionModel &tmodel,
    const WordAlignLatticeLexiconInfo &lexicon_info,
    const fst::SymbolTable *word_syms,
    const Lattice &lat,
    int64 tot_samples) {
  if (!end_of_utterance) {
    LatticeWeight weight;
    std::vector<int32> alignment;
    std::vector<int32> words;
    GetLinearSymbolSequence(lat, &alignment, &words, &weight);

    if (word_syms != NULL) {
      std::string result = "";
      for (size_t i = 0; i < words.size(); i++) {
        std::string s = word_syms->Find(words[i]);
        if (s != "") {
          if (result != "") result += " ";
          result += s;
        }
      }
      if (result != "") {
        result = "PARTIAL:" + result;
        WriteLine(socket, result);
        KALDI_VLOG(1) << "Partial result: " << result;
      }
    }
  } else {
    std::vector<int32> words, times, lengths;

    CompactLattice best_path_clat;
    CompactLattice aligned_lat;
    ConvertLattice(lat, &best_path_clat);

    CompactLattice aligned_clat;
    WordAlignLatticeLexiconOpts opts;
    bool ok = WordAlignLatticeLexicon(best_path_clat, tmodel, lexicon_info,
                                      opts, &aligned_clat);
    TopSortCompactLatticeIfNeeded(&aligned_clat);
    CompactLatticeToWordAlignment((ok ? aligned_clat : best_path_clat), &words,
                                  &times, &lengths);

    int32 words_num = 0;
    for (size_t i = 0; i < words.size(); i++) {
      if (words[i] != 0)
        words_num++;
    }

    float dur = (clock() - start_time) / static_cast<float> (CLOCKS_PER_SEC);
    float input_dur = tot_samples / 16000.0;

    std::stringstream sstr;
    sstr << "RESULT:NUM=" << words_num << ",FORMAT=WSE,RECO-DUR=" << dur
         << ",INPUT-DUR=" << input_dur;
    WriteLine(socket, sstr.str());
    KALDI_VLOG(1) << sstr.str();

    std::string result = "";
    for (size_t i = 0; i < words.size(); i++) {
      if (words[i] == 0)
        continue;   // skip silences...

      std::string word;
      if (word_syms != NULL) word = word_syms->Find(words[i]);
      if (word.empty()) {
        word = "???";
      } else {
        if (result != "") result += " ";
        result += word;
      }

      float start = times[i] * secs_per_frame;
      float len = lengths[i] * secs_per_frame;

      std::stringstream wstr;
      wstr << "RESULT:WORD=" << word << "," << start << "," << (start + len);

      WriteLine(socket, wstr.str());
      KALDI_VLOG(1) << wstr.str();
    }
    if (result != "") {
      KALDI_VLOG(1) << "FINAL result: " << result;
    }
  }
}

fst::Fst<fst::StdArc> *ReadFstKaldi_(std::string rxfilename) {
  if (rxfilename == "") rxfilename = "-";  // interpret "" as stdin,
  // for compatibility with OpenFst conventions.
  kaldi::Input ki(rxfilename);
  fst::FstHeader hdr;
  if (!hdr.Read(ki.Stream(), rxfilename))
    KALDI_ERR << "Reading FST: error reading FST header from "
              << kaldi::PrintableRxfilename(rxfilename);
  fst::FstReadOptions ropts("<unspecified>", &hdr);
  KALDI_VLOG(1) << "fst type is " << hdr.FstType();
  fst::Fst<fst::StdArc> *fst = fst::Fst<fst::StdArc>::Read(ki.Stream(), ropts);
  if (!fst)
    KALDI_ERR << "Could not read fst from "
              << kaldi::PrintableRxfilename(rxfilename);
  return fst;
}

}  // namespace kaldi

int main(int argc, char *argv[]) {
  mcheck(NULL);
  try {
    typedef kaldi::int32 int32;
    typedef kaldi::int64 int64;

    const char *usage =
        "Receives wav data and simulates online decoding with neural nets\n"
        "(nnet3 setup), with optional iVector-based speaker adaptation and\n"
        "optional endpointing.  This version uses multiple threads for "
        "decoding.\n"
        "Note: some configuration values and inputs are set via config files\n"
        "whose filenames are passed as options\n"
        "\n"
        "Usage: audio-server-online2-nnet3 [options] <lexicon-file> "
        "<nnet3-in> <fst-in>\n";

    kaldi::DecoderPool decoder_pool;
    kaldi::ParseOptions po(usage);

    std::string word_syms_rxfilename;

    // feature_opts includes configuration for the iVector adaptation,
    // as well as the basic features.
    kaldi::OnlineCmvnNnet2FeaturePipelineConfig feature_opts;
    kaldi::OnlineEndpointConfig endpoint_opts;

    bool modify_ivector_config = false;
    int32 server_port_number = 5010;

    po.Register("chunk-length", &chunk_length_secs,
                "Length of chunk size in seconds, that we process.  "
                "Set to <= 0 to use all input in one chunk.");
    po.Register("packet-size", &packet_size,
                "Send this many bytes per packet");
    po.Register("word-symbol-table", &word_syms_rxfilename,
                "Symbol table for words [for debug output]");
    po.Register("modify-ivector-config", &modify_ivector_config,
                "If true, modifies the iVector configuration from the config "
                "files by setting --use-most-recent-ivector=true and "
                "--greedy-ivector-extractor=true. This will give the "
                "best possible results, but the results may become dependent "
                "on the speed of your machine (slower machine -> better "
                "results).  Compare to the --online option in "
                "online2-wav-nnet3-latgen-faster");
    po.Register("num-threads-startup", &kaldi::g_num_threads,
                "Number of threads used when initializing iVector extractor.");
    po.Register("server-port-number", &server_port_number,
                "Tcp based Server port number for accepting tasks");

    decoder_pool._config.Register(&po);

    feature_opts.Register(&po);
    decodable_opts.Register(&po);
    endpoint_opts.Register(&po);

    po.Read(argc, argv);
    secs_per_frame = 0.01 * decodable_opts.frame_subsampling_factor;

    if (po.NumArgs() != 3) {
      po.PrintUsage();
      return 1;
    }

    std::string align_lexicon_rxfilename = po.GetArg(1),
        nnet3_rxfilename = po.GetArg(2),
        fst_rxfilename = po.GetArg(3);

    std::vector<std::vector<int32> > lexicon;
    {
      bool binary_in;
      kaldi::Input ki(align_lexicon_rxfilename, &binary_in);
      KALDI_ASSERT(!binary_in && "Not expecting binary file for lexicon");
      if (!kaldi::ReadLexiconForWordAlign(ki.Stream(), &lexicon)) {
        KALDI_ERR << "Error reading alignment lexicon from "
                  << align_lexicon_rxfilename;
      }
    }

    decoder_pool._lexicon_info =
        new kaldi::WordAlignLatticeLexiconInfo(lexicon);

    decoder_pool._feature_info =
        new kaldi::OnlineCmvnNnet2FeaturePipelineInfo(feature_opts);
    if (modify_ivector_config) {
      decoder_pool._feature_info
          ->ivector_extractor_info.use_most_recent_ivector = true;
      decoder_pool._feature_info
          ->ivector_extractor_info.greedy_ivector_extractor = true;
    }

    {
      bool binary;
      kaldi::Input ki(nnet3_rxfilename, &binary);
      decoder_pool._tmodel.Read(ki.Stream(), binary);
      decoder_pool._am_nnet.Read(ki.Stream(), binary);
      kaldi::nnet3::SetBatchnormTestMode(
          true, &(decoder_pool._am_nnet.GetNnet()));
      kaldi::nnet3::SetDropoutTestMode(
          true, &(decoder_pool._am_nnet.GetNnet()));
      kaldi::nnet3::CollapseModel(
          kaldi::nnet3::CollapseModelConfig(),
            &(decoder_pool._am_nnet.GetNnet()));
    }

    // this object contains precomputed stuff that is used by all decodable
    // objects.  It takes a pointer to am_nnet because if it has iVectors it has
    // to modify the nnet to accept iVectors at intervals.
    kaldi::nnet3::DecodableNnetSimpleLoopedInfo decodable_info(
        decodable_opts, &(decoder_pool._am_nnet));

    decoder_pool._fst = fst::ReadFstKaldiGeneric(fst_rxfilename);
    if (word_syms_rxfilename != "")
      if (!(decoder_pool._word_syms =
          fst::SymbolTable::ReadText(word_syms_rxfilename)))
        KALDI_ERR << "Could not read symbol table from file "
                  << word_syms_rxfilename;

    decoder_pool.Run(kaldi::g_num_threads);

    kaldi::TcpServer tcp_server;
    if (!tcp_server.Listen(server_port_number))
      return 0;

    int testcase_num = 0;
    while (true) {
      decoder_pool.NewTask(tcp_server.Accept());
      // testcase_num++;
      if (testcase_num > 5) {
        while (true) {
          if (!decoder_pool.IsBusy()) break;
          kaldi::Sleep(1.0f);
        }
        break;
      }
    }

    return 0;
  } catch(const std::exception& e) {
    std::cerr << e.what();
    return -1;
  }
  muntrace();
}  // main()

namespace kaldi {

// IMPLEMENTATION OF THE CLASSES/METHODS ABOVE MAIN
TcpServer::TcpServer() {
  _server_desc_ = -1;
}

bool TcpServer::Listen(int32 port) {
  _h_addr_.sin_addr.s_addr = INADDR_ANY;
  _h_addr_.sin_port = htons(port);
  _h_addr_.sin_family = AF_INET;

  _server_desc_ = socket(AF_INET, SOCK_STREAM, 0);

  if (_server_desc_ == -1) {
    KALDI_ERR << "Cannot create TCP socket!";
    return false;
  }

  int32 flag = 1;
  int32 len = sizeof(int32);
  if (setsockopt(_server_desc_, SOL_SOCKET, SO_REUSEADDR, &flag, len) == -1) {
    KALDI_ERR << "Cannot set socket options!\n";
    return false;
  }

  if (bind(_server_desc_, (struct sockaddr*) &_h_addr_, sizeof(_h_addr_))
      == -1) {
    KALDI_ERR << "Cannot bind to port: " << port << " (is it taken?)";
    return false;
  }

  if (listen(_server_desc_, 1) == -1) {
    KALDI_ERR << "Cannot listen on port!";
    return false;
  }

  KALDI_VLOG(1) << "TcpServer: Listening on port: " << port;

  return true;
}

TcpServer::~TcpServer() {
  if (_server_desc_ != -1)
    close(_server_desc_);
}

int32 TcpServer::Accept() {
  KALDI_VLOG(1) << "Waiting for client...";

  socklen_t len;

  len = sizeof(struct sockaddr);
  int32 client_desc = accept(_server_desc_, (struct sockaddr*) &_h_addr_, &len);

  struct sockaddr_storage addr;
  char ipstr[20];

  len = sizeof addr;
  getpeername(client_desc, (struct sockaddr*) &addr, &len);

  struct sockaddr_in *s = (struct sockaddr_in *) &addr;
  inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);

  KALDI_VLOG(1) << "TcpServer: Accepted connection from: " << ipstr;
  signal(SIGPIPE, SIG_IGN);

  return client_desc;
}

DecoderPool::DecoderPool() {
  _num = 0;
  _decoder_threads = NULL;
  _fst = NULL;
  _feature_info = NULL;
  _word_syms = NULL;
  _lexicon_info = NULL;
}

DecoderPool::~DecoderPool() {
  if (_fst != NULL) delete _fst;
  if (_feature_info != NULL) delete _feature_info;
  if (_word_syms != NULL) delete _word_syms;
  if (_decoder_threads != NULL) delete[] _decoder_threads;
  if (_lexicon_info != NULL) delete _lexicon_info;
}

void* DecoderPool::ThreadProc(void* para) {
  DecoderThread* dt = reinterpret_cast <DecoderThread*> (para);
  KALDI_ASSERT(dt != NULL);
  KALDI_ASSERT(dt->_pool != NULL);
  KALDI_VLOG(1) << "Decoder " << dt->_tid << " is ready";

  while (true) {
    if (dt->_client_socket == -1) {
      Sleep(0.1f);
      continue;
    }
    pthread_mutex_lock(&(dt->_lock));
    KALDI_VLOG(1) << "Decoder " << dt->_tid << " is running";
    while (true) {
      OnlineCmvnNnet2FeaturePipeline feature_pipeline(
          *dt->_pool->_feature_info);
      // this object contains precomputed stuff that is used by all decodable
      // objects.  It takes a pointer to am_nnet because if it has iVectors it
      // has to modify the nnet to accept iVectors at intervals.
      nnet3::DecodableNnetSimpleLoopedInfo decodable_info(decodable_opts,
                                                          &dt->_pool->_am_nnet);
      SingleUtteranceNnet3Decoder decoder(
          dt->_pool->_config, dt->_pool->_tmodel, decodable_info,
          *dt->_pool->_fst, &feature_pipeline);

      OnlineTcpVectorSource au_src(dt->_client_socket);
      std::string utt = "";
      BaseFloat samp_freq = 16000;
      int32 chunk_length;
      if (chunk_length_secs > 0) {
        chunk_length = static_cast <int32> (samp_freq * chunk_length_secs);
        if (chunk_length == 0) chunk_length = 1;
      } else {
        chunk_length = std::numeric_limits<int32>::max();
      }

      int32 start_time = clock();

      OnlineTimer decoding_timer(utt);
      int32 samp_offset = 0, samp_partial = 0, samp_process = 0;
      Lattice lat;
      bool end_of_utterance;

      // Client loop to receive wav data
      while (true) {
        Vector<BaseFloat> wav_data(packet_size / 2);
        bool ans = au_src.Read(&wav_data);

        feature_pipeline.AcceptWaveform(samp_freq, wav_data);

        if (ans) samp_offset += packet_size / 2;
        // by introducing minor delay, you'll get speedup.
        if (samp_offset - samp_process < chunk_length && ans) continue;
        samp_process = samp_offset;
        decoder.AdvanceDecoding();

        if (samp_offset - samp_partial > 0.3 * samp_freq
            && decoder.NumFramesDecoded() > 0) {
          samp_partial = samp_offset;
          end_of_utterance = false;
          decoder.GetBestPath(end_of_utterance, &lat);
          GetDiagnosticsAndPrintOutput(
              dt->_client_socket, end_of_utterance, start_time,
              utt, dt->_pool->_tmodel, *dt->_pool->_lexicon_info,
              dt->_pool->_word_syms, lat, samp_offset);
        }
        if (!ans) break;
      }
      if (samp_offset == 0) {
        KALDI_VLOG(1) << "Decoder " << dt->_tid << " break";
        break;
      }

      feature_pipeline.InputFinished();
      Timer timer;
      decoder.AdvanceDecoding();
      decoder.FinalizeDecoding();

      end_of_utterance = true;
      decoder.GetBestPath(end_of_utterance, &lat);
      GetDiagnosticsAndPrintOutput(
          dt->_client_socket, end_of_utterance, start_time,
          utt, dt->_pool->_tmodel, *dt->_pool->_lexicon_info,
          dt->_pool->_word_syms, lat, samp_offset);
      KALDI_VLOG(1) << "Decoder " << dt->_tid << " finished";
      WriteLine(dt->_client_socket, "RESULT:DONE");
    }
    close(dt->_client_socket);
    dt->_is_free = true;
    dt->_client_socket = -1;
    pthread_mutex_unlock(&(dt->_lock));
  }

  return reinterpret_cast <void*> (NULL);
}

void DecoderPool::Run(const int32 &n) {
  int32 i;

  _num = n;
  _decoder_threads = new DecoderThread[_num];
  KALDI_ASSERT(_decoder_threads != NULL);

  for (i = 0; i < _num; i++) {
    _decoder_threads[i]._pool = this;

    pthread_mutex_init(&(_decoder_threads[i]._lock), NULL);
    _decoder_threads[i]._is_free = true;
    _decoder_threads[i]._client_socket = -1;
  }

  for (i = 0; i < _num; i++) {
    int32 err = pthread_create(&(_decoder_threads[i]._tid),
                                NULL,
                                DecoderPool::ThreadProc,
                                &(_decoder_threads[i]));
    if (err != 0) {
      KALDI_LOG << "Can't create thread " << i << ": " << strerror(err);
    }
  }
}

void DecoderPool::NewTask(int32 client_socket) {
  for (int32 i = 0; i < _num; i++) {
    if (_decoder_threads[i]._is_free) {
      KALDI_VLOG(1) << "Decoder " << _decoder_threads[i]._tid \
          << " is free to used";
      pthread_mutex_lock(&(_decoder_threads[i]._lock));
      _decoder_threads[i]._is_free = false;
      _decoder_threads[i]._client_socket = client_socket;
      pthread_mutex_unlock(&(_decoder_threads[i]._lock));
      return;
    }
  }
  close(client_socket);
}

bool DecoderPool::IsBusy() {
  for (int32 i = 0; i < _num; i++) {
    if (!_decoder_threads[i]._is_free) {
      return true;
    }
  }
  return false;
}

}  // namespace kaldi
