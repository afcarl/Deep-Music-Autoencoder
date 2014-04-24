#include "ae.hpp"
#include <algorithm>
#include <iostream>
#include "ps.hpp"
#include "utils.hpp"

namespace paracel{

// construction function
autoencoder::autoencoder(paracel::Comm comm, string hosts_dct_str,
          string _input, string output, vector<int> _hidden_size,
          vector<int> _visible_size, string method, int _rounds, 
          double _alpha, bool _debug, int limit_s, bool ssp_switch, 
          double _lamb, double _sparsity_param, double _beta, int _mibt_size) :
  paracel::paralg(hosts_dct_str, comm, output, _rounds, limit_s, ssp_switch),
  worker_id(comm.get_rank()),
  input(_input),
  learning_method(method),
  rounds(_rounds),
  alpha(_alpha),
  debug(_debug),
  lamb(_lamb),
  sparsity_param(_sparsity_param),
  beta(_beta),
  mibt_size(_mibt_size) {
    assert(_hidden_size.size() == _visible_size.size());
    n_lyr = _hidden_size.size();
    hidden_size.assign(_hidden_size.begin(), _hidden_size.end());
    visible_size.assign(_visible_size.begin(), _visible_size.end());
    ae_init();
  }


autoencoder::~autoencoder() {}

// distributed bgd
void autoencoder::distribute_bgd(int lyr){
  unordered_map<string, MatrixXd>& WgtBias_lyr = WgtBias[lyr] ;
  _paracel_write("W1", WgtBias_lyr.at("W1"));
  _paracel_write("W2", WgtBias_lyr.at("W2"));
  _paracel_write("b1", WgtBias_lyr.at("b1"));
  _paracel_write("b2", WgtBias_lyr.at("b2"));
  paracel_register_bupdate("./update.so", 
      "ae_update");
  unordered_map<string, MatrixXd> delta;
  delta["W1"] = MatrixXd::Zero(WgtBias[lyr].at("W1").rows(), WgtBias[lyr].at("W1").cols());
  delta["W2"] = MatrixXd::Zero(WgtBias[lyr].at("W2").rows(), WgtBias[lyr].at("W2").cols());
  delta["b1"] = VectorXd::Zero(WgtBias[lyr].at("b1").size());
  delta["b2"] = VectorXd::Zero(WgtBias[lyr].at("b2").size());
  for (int rd = 0; rd < rounds; rd++) {
    WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
    WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
    WgtBias_lyr.at("b1") = _paracel_read("b1");
    WgtBias_lyr.at("b2") = _paracel_read("b2");
    delta = ae_batch_grad(lyr);
    delta.at("W1") *= alpha;
    delta.at("W2") *= alpha;
    delta.at("b1") *= alpha;
    delta.at("b2") *= alpha;
    if (debug) {
      loss_error.push_back(ae_cost(lyr));
    }
    // push
    _paracel_bupdate("W1", delta.at("W1"));
    _paracel_bupdate("W2", delta.at("W2"));
    _paracel_bupdate("b1", delta.at("b1"));
    _paracel_bupdate("b2", delta.at("b2"));
    iter_commit();
    std::cout << "worker" << get_worker_id() << "at the end of rd" << rd << std::endl;
  } // rounds
  // last pull
  WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
  WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
  WgtBias_lyr.at("b1") = _paracel_read("b1");
  WgtBias_lyr.at("b2") = _paracel_read("b2");
}


// downpour sgd
void autoencoder::downpour_sgd(int lyr){
  int data_sz = data.cols();
  int cnt = 0, read_batch = data_sz/ 1000, update_batch = data_sz / 100;
  assert( (lyr > 0 && lyr < n_lyr) && "Input layer not qualified!");
  if (read_batch == 0) { read_batch = 10; }
  if (update_batch == 0) { update_batch = 10; }
  // Reference operator
  unordered_map<string, MatrixXd>& WgtBias_lyr = WgtBias[lyr] ;
  _paracel_write("W1", WgtBias_lyr.at("W1"));
  _paracel_write("W2", WgtBias_lyr.at("W2"));
  _paracel_write("b1", WgtBias_lyr.at("b1"));
  _paracel_write("b2", WgtBias_lyr.at("b2"));
  vector<int> idx;
  for (int i = 0; i < data.cols(); i++) {
    idx.push_back(i);
  }
  paracel_register_bupdate("./update.so", 
      "ae_update");
  unordered_map<string, MatrixXd> delta;
  delta["W1"] = MatrixXd::Zero(WgtBias[lyr].at("W1").rows(), WgtBias[lyr].at("W1").cols());
  delta["W2"] = MatrixXd::Zero(WgtBias[lyr].at("W2").rows(), WgtBias[lyr].at("W2").cols());
  delta["b1"] = VectorXd::Zero(WgtBias[lyr].at("b1").size());
  delta["b2"] = VectorXd::Zero(WgtBias[lyr].at("b2").size());

  for (int rd = 0; rd < rounds; rd++) {
    std::random_shuffle(idx.begin(), idx.end());

    // init read
    WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
    WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
    WgtBias_lyr.at("b1") = _paracel_read("b1");
    WgtBias_lyr.at("b2") = _paracel_read("b2");
    unordered_map<string, MatrixXd> WgtBias_lyr_old(WgtBias_lyr);

    // traverse data
    cnt = 0;
    for (auto sample_id : idx) {
      if ( (cnt % read_batch == 0) || (cnt == (int)idx.size() - 1) ) {
        WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
        WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
        WgtBias_lyr.at("b1") = _paracel_read("b1");
        WgtBias_lyr.at("b2") = _paracel_read("b2");
        WgtBias_lyr_old = WgtBias_lyr;
      }
      unordered_map<string, MatrixXd> WgtBias_grad = ae_stoc_grad(lyr, sample_id);
      WgtBias_lyr.at("W1") += alpha * WgtBias_grad["W1"];
      WgtBias_lyr.at("W2") += alpha * WgtBias_grad["W2"];
      WgtBias_lyr.at("b1") += alpha * WgtBias_grad["b1"];
      WgtBias_lyr.at("b2") += alpha * WgtBias_grad["b2"];
      if (debug) {
        loss_error.push_back(ae_cost(lyr));
      }
      if ( (cnt % update_batch == 0) || (cnt == (int)idx.size() - 1) ) {
        delta.at("W1") = WgtBias_lyr.at("W1") - WgtBias_lyr_old.at("W1");
        delta.at("W2") = WgtBias_lyr.at("W2") - WgtBias_lyr_old.at("W2");
        delta.at("b1") = WgtBias_lyr.at("b1") - WgtBias_lyr_old.at("b1");
        delta.at("b2") = WgtBias_lyr.at("b2") - WgtBias_lyr_old.at("b2");
        // push
        _paracel_bupdate("W1", delta.at("W1"));
        _paracel_bupdate("W2", delta.at("W2"));
        _paracel_bupdate("b1", delta.at("b1"));
        _paracel_bupdate("b2", delta.at("b2"));
      }
      cnt += 1;
    } // traverse
    sync();
    std::cout << "worker" << get_worker_id() << "at the end of rd" << rd << std::endl;
  }  // rounds
  // last pull
  WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
  WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
  WgtBias_lyr.at("b1") = _paracel_read("b1");
  WgtBias_lyr.at("b2") = _paracel_read("b2");
}


// mini-batch downpour sgd
void autoencoder::downpour_sgd_mibt(int lyr){
  int data_sz = data.cols();
  int mibt_cnt = 0, read_batch = data_sz / (mibt_size*100), update_batch = data_sz / (mibt_size*100);
  assert( (lyr > 0 && lyr < n_lyr) && "Input layer not qualified!");
  if (read_batch == 0) { read_batch = 10; }
  if (update_batch == 0) { update_batch = 10; }
  // Reference operator
  unordered_map<string, MatrixXd>& WgtBias_lyr = WgtBias[lyr] ;
  _paracel_write("W1", WgtBias_lyr.at("W1"));
  _paracel_write("W2", WgtBias_lyr.at("W2"));
  _paracel_write("b1", WgtBias_lyr.at("b1"));
  _paracel_write("b2", WgtBias_lyr.at("b2"));
  vector<int> idx;
  for (int i = 0; i < data.cols(); i++) {
    idx.push_back(i);
  }
  // ABSOULTE PATH
  paracel_register_bupdate("./update.so", 
      "ae_update");
  unordered_map<string, MatrixXd> delta;
  delta["W1"] = MatrixXd::Zero(WgtBias[lyr].at("W1").rows(), WgtBias[lyr].at("W1").cols());
  delta["W2"] = MatrixXd::Zero(WgtBias[lyr].at("W2").rows(), WgtBias[lyr].at("W2").cols());
  delta["b1"] = VectorXd::Zero(WgtBias[lyr].at("b1").size());
  delta["b2"] = VectorXd::Zero(WgtBias[lyr].at("b2").size());

  for (int rd = 0; rd < rounds; rd++) {
    std::random_shuffle(idx.begin(), idx.end());
    vector<vector<int>> mibt_idx; // mini-batch id
    for (auto i = idx.begin(); ; i += mibt_size) {
      if (idx.end() - i < mibt_size) {
        if (idx.end() == i) {
          break;
        }else{
          vector<int> tmp;
          tmp.assign(i, idx.end());
          mibt_idx.push_back(tmp);
        }
      }
      vector<int> tmp;
      tmp.assign(i, i + mibt_size);
      // SUPPOSE IT TO BE NOT ACCUMULATED OVER WORKERS?
      mibt_idx.push_back(tmp);
    }
    // init push
    WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
    WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
    WgtBias_lyr.at("b1") = _paracel_read("b1");
    WgtBias_lyr.at("b2") = _paracel_read("b2");
    unordered_map<string, MatrixXd> WgtBias_lyr_old(WgtBias_lyr);
    
    // traverse data
    mibt_cnt = 0;
    for (auto mibt_sample_id : mibt_idx) {
      if ( (mibt_cnt % read_batch == 0) || (mibt_cnt == (int)mibt_idx.size()-1) ) {
        WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
        WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
        WgtBias_lyr.at("b1") = _paracel_read("b1");
        WgtBias_lyr.at("b2") = _paracel_read("b2");
        WgtBias_lyr_old = WgtBias_lyr;
      }
      unordered_map<string, MatrixXd> WgtBias_grad = ae_mibt_stoc_grad(lyr, mibt_sample_id);
      WgtBias_lyr.at("W1") += alpha * WgtBias_grad["W1"];
      WgtBias_lyr.at("W2") += alpha * WgtBias_grad["W2"];
      WgtBias_lyr.at("b1") += alpha * WgtBias_grad["b1"];
      WgtBias_lyr.at("b2") += alpha * WgtBias_grad["b2"];
      if (debug) {
        loss_error.push_back(ae_cost(lyr));
      }
      if ( (mibt_cnt % update_batch == 0) || (mibt_cnt == (int)mibt_idx.size()-1) ) {
        delta.at("W1") = WgtBias_lyr.at("W1") - WgtBias_lyr_old.at("W1");
        delta.at("W2") = WgtBias_lyr.at("W2") - WgtBias_lyr_old.at("W2");
        delta.at("b1") = WgtBias_lyr.at("b1") - WgtBias_lyr_old.at("b1");
        delta.at("b2") = WgtBias_lyr.at("b2") - WgtBias_lyr_old.at("b2");
        // push
        _paracel_bupdate("W1", delta.at("W1"));
        _paracel_bupdate("W2", delta.at("W2"));
        _paracel_bupdate("b1", delta.at("b1"));
        _paracel_bupdate("b2", delta.at("b2"));
      }
      mibt_cnt += 1;
    }  // traverse
    sync();
    std::cout << "worker" << get_worker_id() << "at the end of rd" << rd << std::endl;
  }  // rounds
  // last pull
  WgtBias_lyr.at("W1") = _paracel_read("W1", WgtBias_lyr("W1").rows(), WgtBias_lyr("W1").cols());
  WgtBias_lyr.at("W2") = _paracel_read("W2", WgtBias_lyr("W2").rows(), WgtBias_lyr("W2").cols());
  WgtBias_lyr.at("b1") = _paracel_read("b1");
  WgtBias_lyr.at("b2") = _paracel_read("b2");
}


void autoencoder::train(int lyr){
  int i;
  auto lines = paracel_load(input);
  local_parser(lines); 
  sync();
  if (learning_method == "dbgd") {
    std::cout << "chose distributed batch gradient descent" << std::endl;
    set_total_iters(rounds); // default value
    for (i = 0; i < n_lyr; i++) {
      distribute_bgd(lyr);
    }
  } else if (learning_method == "dsgd") {
    std::cout << "chose downpour stochasitc gradient descent" << std::endl;
    set_total_iters(rounds); // default value
    for (i = 0; i < n_lyr; i++) {
      downpour_sgd(lyr);
    }
  } else if (learning_method == "mbdsgd") {
    std::cout << "chose mini-batch downpour stochastic gradient descent" << std::endl;
    set_total_iters(rounds); // default value
    for (i = 0; i < n_lyr; i++) {
      downpour_sgd_mibt(lyr);
    }
  } else {
    std::cout << "learning method not supported." << std::endl;
    return;
  }
  sync();
}


void autoencoder::train(){
  // top function
  for (int i = 0; i < n_lyr; i++) {
    train(i);
    dump_result(i);
  }
  std::cout << "Mission complete" << std::endl;
}


void autoencoder::local_parser(const vector<string> & linelst, const char sep, bool spv){
  samples.resize(0);
  labels.resize(0);
  if (spv) {  // supervised
    for (auto & line: linelst) {
      vector<double> tmp;
      auto linev = paracel::str_split(line, sep);
      // WHY???
      tmp.push_back(1.);  
      for (size_t i = 0; i < linev.size() - 1; i++) {
        tmp.push_back(std::stod(linev[i]));
      }
      samples.push_back(tmp);
      labels.push_back(std::stod(linev.back()));
    } // traverse file
  } else {  // unsupervised
    for (auto & line : linelst) {
      vector<double> tmp;
      auto linev = paracel::str_split(line, sep);
      // WHY??
      tmp.push_back(1.);
      for (size_t i = 0; i < linev.size(); i++) {
        tmp.push_back(std::stod(linev[i]));
      }
      samples.push_back(tmp);
    }
  }
  data = vec_to_mat(samples).transpose();  // transpose is needed, since the data is sliced by-row 
                                 // and samples are stored by-column in variable "data".
}


/*
void autoencoder::dump_result(int lyr){
  int i;
  if (get_worker_id() == 0) {
    for (i = 0; i < WgtBias[lyr]["W1"].rows(); i++) {
      paracel_dump_vector(Vec_to_vec(WgtBias[lyr]["W1"].row(i)), 
            ("ae_layer_" + std::to_string(lyr) + "_W1_"), ",", false);
    }
    for (i = 0; i < WgtBias[lyr]["W2"].rows(); i++) {
      paracel_dump_vector(Vec_to_vec(WgtBias[lyr]["W2"].row(i)), 
            ("ae_layer_" + std::to_string(lyr) + "_W2_"), ",", false);
    }
    for (i = 0; i < WgtBias[lyr]["b1"].rows(); i++) {
      paracel_dump_vector(Vec_to_vec(WgtBias[lyr]["b1"].row(i)), 
            ("ae_layer_" + std::to_string(lyr) + "_b1_"), ",", false);
    }
    for (i = 0; i < WgtBias[lyr]["b2"].rows(); i++) {
      paracel_dump_vector(Vec_to_vec(WgtBias[lyr]["b2"].row(i)), 
            ("ae_layer_" + std::to_string(lyr) + "_b2_"), ",", false);
    }
  }
}*/

void _paracel_write(string key, MatrixXd & m){
  paracel_write(key, Mat_to_vec(m));
}

MatrixXd _paracel_read(string key, int r, int c){
  vector<double> v = paracel_read<vector<double> >(key);
  return vec_to_mat(v, r, c);
}

VectorXd _paracel_read(string key){
  vector<double> v = paracel_read<vector<double> >(key);
  return vec_to_mat(v);
}

void _paracel_bupdate(string key, MatrixXd & m){
  paracel_bupdate(key, Mat_to_vec(m));
}


} // namespace paracel


//MatrixXd vec_to_mat(vector< vector<double> > & v) {
//  MatrixXd m(v.size(), v[0].size());
//  for (int i = 0; i < v.size(); i++) {
//    m.row(i) = VectorXd::Map(&v[i][0], v[i].size());  // row ordered
//  }
//  return m;
//}


VectorXd vec_to_mat(vector<double> & v) {
  VectorXd m(v.size());
  m = VectorXd::Map(&v[0], v.size()); // column ordered
  return m;
}

MatrixXd vec_to_mat(vector<double> & v, int r, int c){
  assert(v.size() == r * c);
  MatrixXd m(r, c);
  m = MatrixXd::Map(&v[0], r, c); // column ordered
  return m;
}

vector<double> Mat_to_vec(MatrixXd & m){
  vector<double> v(m.size());
  // column ordered
  Eigen::Map<MatrixXd>(v.data(), m.rows(), m.cols()) = m;
  return v;
}


