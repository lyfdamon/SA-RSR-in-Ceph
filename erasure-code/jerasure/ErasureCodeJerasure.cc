// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 * 
 */

#include "common/debug.h"
#include "ErasureCodeJerasure.h"
#include "crush/CrushWrapper.h"
#include "osd/osd_types.h"
extern "C" {
#include "jerasure.h"
#include "reed_sol.h"
#include "galois.h"
#include "cauchy.h"
#include "liberation.h"
#include "cauchy.h"
}

#define LARGEST_VECTOR_WORDSIZE 16

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodeJerasure: ";
}

int ErasureCodeJerasure::create_ruleset(const string &name,
					CrushWrapper &crush,
					ostream *ss) const
{
  int ruleid = crush.add_simple_ruleset(name, ruleset_root, ruleset_failure_domain,
					"indep", pg_pool_t::TYPE_ERASURE, ss);
  if (ruleid < 0)
    return ruleid;
  else {
    crush.set_rule_mask_max_size(ruleid, get_chunk_count());
    return crush.get_rule_mask_ruleset(ruleid);
  }
}

int ErasureCodeJerasure::init(ErasureCodeProfile& profile, ostream *ss)
{
  int err = 0;
  dout(10) << "technique=" << technique << dendl;
  profile["technique"] = technique;
  err |= to_string("ruleset-root", profile,
		   &ruleset_root,
		   DEFAULT_RULESET_ROOT, ss);
  err |= to_string("ruleset-failure-domain", profile,
		   &ruleset_failure_domain,
		   DEFAULT_RULESET_FAILURE_DOMAIN, ss);
  err |= parse(profile, ss);
  if (err)
    return err;
  prepare();
  ErasureCode::init(profile, ss);
  return err;
}

int ErasureCodeJerasure::parse(ErasureCodeProfile &profile,
			       ostream *ss)
{
  int err = ErasureCode::parse(profile, ss);
  err |= to_int("k", profile, &k, DEFAULT_K, ss);
  err |= to_int("m", profile, &m, DEFAULT_M, ss);
  err |= to_int("w", profile, &w, DEFAULT_W, ss);
  if (chunk_mapping.size() > 0 && (int)chunk_mapping.size() != k + m) {
    *ss << "mapping " << profile.find("mapping")->second
	<< " maps " << chunk_mapping.size() << " chunks instead of"
	<< " the expected " << k + m << " and will be ignored" << std::endl;
    chunk_mapping.clear();
    err = -EINVAL;
  }
  err |= sanity_check_k(k, ss);
  return err;
}

unsigned int ErasureCodeJerasure::get_chunk_size(unsigned int object_size) const
{
  unsigned alignment = get_alignment();
  if (per_chunk_alignment) {
    unsigned chunk_size = object_size / k;
    if (object_size % k)
      chunk_size++;
    dout(20) << "get_chunk_size: chunk_size " << chunk_size
	     << " must be modulo " << alignment << dendl; 
    assert(alignment <= chunk_size);
    unsigned modulo = chunk_size % alignment;
    if (modulo) {
      dout(10) << "get_chunk_size: " << chunk_size
	       << " padded to " << chunk_size + alignment - modulo << dendl;
      chunk_size += alignment - modulo;
    }
    return chunk_size;
  } else {
    unsigned tail = object_size % alignment;
    unsigned padded_length = object_size + ( tail ?  ( alignment - tail ) : 0 );
    assert(padded_length % k == 0);
    return padded_length / k;
  }
}

int ErasureCodeJerasure::encode_chunks(const set<int> &want_to_encode,
				       map<int, bufferlist> *encoded)
{
  char *chunks[k + m];
  for (int i = 0; i < k + m; i++)
    chunks[i] = (*encoded)[i].c_str();
  jerasure_encode(&chunks[0], &chunks[k], (*encoded)[0].length());
  return 0;
}

int ErasureCodeJerasure::decode_chunks(const set<int> &want_to_read,
				       const map<int, bufferlist> &chunks,
				       map<int, bufferlist> *decoded)
{
  unsigned blocksize = (*chunks.begin()).second.length();
  int erasures[k + m + 1];
  int erasures_count = 0;
  char *data[k];
  char *coding[m];
  for (int i =  0; i < k + m; i++) {
    if (chunks.find(i) == chunks.end()) {
      erasures[erasures_count] = i;
      erasures_count++;
    }
    if (i < k)
      data[i] = (*decoded)[i].c_str();
    else
      coding[i - k] = (*decoded)[i].c_str();
  }
  erasures[erasures_count] = -1;

  assert(erasures_count > 0);
  return jerasure_decode(erasures, data, coding, blocksize);
}

void ErasureCodeJerasure::get_Control(int k, int w, map<int, vector<int> > solution, Control* control)
{
  control->node_numbers = k + 1;
  control->solution = (Solution**)malloc(sizeof(Solution*) * control->node_numbers);
  Node_info* node_info = (Node_info*)malloc(sizeof(Node_info) + sizeof(int) * w);
  node_info->symbol_numbers = w;
  int index = 0;
  for (map<int, vector<int> >::iterator solution_iter = solution.begin(); solution_iter != solution.end(); ++solution_iter) 
  {
    if (solution_iter->first < k) 
    {
      control->solution[solution_iter->first] = (Solution*)malloc(sizeof(Solution));
      control->solution[solution_iter->first]->node_id = solution_iter->first;
      control->solution[solution_iter->first]->node_info = (Node_info*)malloc(sizeof(Node_info) + sizeof(int)*solution_iter->second.size());
      control->solution[solution_iter->first]->node_info->symbol_numbers = solution_iter->second.size();
      for (int i = 0; i < control->solution[solution_iter->first]->node_info->symbol_numbers; ++i) 
      {
        control->solution[solution_iter->first]->node_info->symbol_ids[i] = solution_iter->second[i];
      }
    }
    else 
    {
      for (size_t i = 0; i < solution_iter->second.size(); ++i) 
      {
        node_info->symbol_ids[index] = index;
        ++index;
      }
    }
  }
  for (int i = 0; i < k; i++) 
  {
    if (solution.find(i) == solution.end()) 
    {
      control->solution[i] = (Solution*)malloc(sizeof(Solution));
      control->solution[i]->node_id = i;
      control->solution[i]->node_info = node_info;
      control->solution[k] = (Solution*)malloc(sizeof(Solution));
      control->solution[k]->node_id = i;
      control->solution[k]->node_info = node_info;
      break;
    }
  }
}
int ErasureCodeJerasure::decode_chunks_for_xor(const set<int> &want_to_read, const map<int, bufferlist> &chunks, map<int, bufferlist> *decoded, unsigned blocksize, int packet_size, int w, map<int,vector<int> > solution, int* parity_group_selection)
{
  int erasures[k + m + 1];
  int erasures_count = 0;
  char *data[k];
  char *new_coding;
  bufferlist bl_append;
  bufferlist bf;
  bl_append.clear();
  for(unsigned off_packet = 0; off_packet < blocksize; off_packet += packet_size * w)
  {
    for(map<int,vector<int> >::iterator solution_iterator = solution.begin(); solution_iterator != solution.end(); ++solution_iterator)
    {
      if(solution_iterator->first >= k)
      {
        for(unsigned symbol_ids = 0; symbol_ids < solution_iterator->second.size(); ++symbol_ids)
        {
          bf.clear(); 
          bf.substr_of(decoded->find(solution_iterator->first)->second, off_packet * solution_iterator->second.size() / w + symbol_ids * packet_size, packet_size);
          bl_append.claim_append(bf);
        }
      }
    }
  }
  new_coding = bl_append.c_str(); 
  for (int i =  0; i < k + m; i++) 
  {
    if (i < k)
      data[i] = (*decoded)[i].c_str();
  }
  for(set<int>::iterator want_to_read_iter = want_to_read.begin(); want_to_read_iter != want_to_read.end(); ++want_to_read_iter)
  {
     erasures[erasures_count] = *want_to_read_iter;
     erasures_count++;
  }
  erasures[erasures_count] = -1;
  assert(erasures_count > 0);
  Control* control = (Control*)malloc(sizeof(Control));
  get_Control(k, w, solution, control);
  return jerasure_decode_for_xor(erasures, data, new_coding, blocksize, parity_group_selection, control);
}

int* ErasureCodeJerasure::get_bitmatrix()
{
  return get_matrix();//add by LYF
}

int ErasureCodeJerasure::get_packetsize()
{
  return get_symbol_size();//add by LYF
}

bool ErasureCodeJerasure::is_prime(int value)
{
  int prime55[] = {
    2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,
    73,79,83,89,97,101,103,107,109,113,127,131,137,139,149,
    151,157,163,167,173,179,
    181,191,193,197,199,211,223,227,229,233,239,241,251,257
  };
  int i;
  for (i = 0; i < 55; i++)
    if (value == prime55[i])
      return true;
  return false;
}

// 
// ErasureCodeJerasureReedSolomonVandermonde
//
void ErasureCodeJerasureReedSolomonVandermonde::jerasure_encode(char **data,
                                                                char **coding,
                                                                int blocksize)
{
  jerasure_matrix_encode(k, m, w, matrix, data, coding, blocksize);
}

int ErasureCodeJerasureReedSolomonVandermonde::jerasure_decode(int *erasures,
                                                                char **data,
                                                                char **coding,
                                                                int blocksize)
{
  return jerasure_matrix_decode(k, m, w, matrix, 1,
				erasures, data, coding, blocksize);
}

int ErasureCodeJerasureReedSolomonVandermonde::jerasure_decode_for_xor(int *erasures,
                                                                char **data,
                                                                char *coding,
                                                                int blocksize,
                                                                int* parity_group_selection,
                                                                Control* control)//add by LYF
{
  free(control);
  return -1;
}

unsigned ErasureCodeJerasureReedSolomonVandermonde::get_alignment() const
{
  if (per_chunk_alignment) {
    return w * LARGEST_VECTOR_WORDSIZE;
  } else {
    unsigned alignment = k*w*sizeof(int);
    if ( ((w*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
      alignment = k*w*LARGEST_VECTOR_WORDSIZE;
    return alignment;
  }
}

int ErasureCodeJerasureReedSolomonVandermonde::parse(ErasureCodeProfile &profile,
						     ostream *ss)
{
  int err = 0;
  err |= ErasureCodeJerasure::parse(profile, ss);
  if (w != 8 && w != 16 && w != 32) {
    *ss << "ReedSolomonVandermonde: w=" << w
	<< " must be one of {8, 16, 32} : revert to " << DEFAULT_W << std::endl;
    profile["w"] = "8";
    err |= to_int("w", profile, &w, DEFAULT_W, ss);
    err = -EINVAL;
  }
  err |= to_bool("jerasure-per-chunk-alignment", profile,
		 &per_chunk_alignment, "false", ss);
  return err;
}

void ErasureCodeJerasureReedSolomonVandermonde::prepare()
{
  matrix = reed_sol_vandermonde_coding_matrix(k, m, w);
}

int* ErasureCodeJerasureReedSolomonVandermonde::get_matrix()//add by LYF
{
  return matrix;
}

int ErasureCodeJerasureReedSolomonVandermonde::get_symbol_size()//add by LYF
{
  return -1;
}

// 
// ErasureCodeJerasureReedSolomonRAID6
//
void ErasureCodeJerasureReedSolomonRAID6::jerasure_encode(char **data,
                                                                char **coding,
                                                                int blocksize)
{
  reed_sol_r6_encode(k, w, data, coding, blocksize);
}

int ErasureCodeJerasureReedSolomonRAID6::jerasure_decode(int *erasures,
							 char **data,
							 char **coding,
							 int blocksize)
{
  return jerasure_matrix_decode(k, m, w, matrix, 1, erasures, data, coding, blocksize);
}

int ErasureCodeJerasureReedSolomonRAID6::jerasure_decode_for_xor(int *erasures,
               char **data,
               char *coding,
               int blocksize,
               int* parity_group_selection,
               Control* control)
{
  free(control);
  return -1;
}

unsigned ErasureCodeJerasureReedSolomonRAID6::get_alignment() const
{
  if (per_chunk_alignment) {
    return w * LARGEST_VECTOR_WORDSIZE;
  } else {
    unsigned alignment = k*w*sizeof(int);
    if ( ((w*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
      alignment = k*w*LARGEST_VECTOR_WORDSIZE;
    return alignment;
  }
}

int ErasureCodeJerasureReedSolomonRAID6::parse(ErasureCodeProfile &profile,
					       ostream *ss)
{
  int err = ErasureCodeJerasure::parse(profile, ss);
  profile.erase("m");
  m = 2;
  if (w != 8 && w != 16 && w != 32) {
    *ss << "ReedSolomonRAID6: w=" << w
	<< " must be one of {8, 16, 32} : revert to 8 " << std::endl;
    profile["w"] = "8";
    err |= to_int("w", profile, &w, DEFAULT_W, ss);
    err = -EINVAL;
  }
  return err;
}

void ErasureCodeJerasureReedSolomonRAID6::prepare()
{
  matrix = reed_sol_r6_coding_matrix(k, w);
}

int* ErasureCodeJerasureReedSolomonRAID6::get_matrix()//add by LYF
{
  return matrix;
}

int ErasureCodeJerasureReedSolomonRAID6::get_symbol_size()//add by LYF
{
  return -1;
}

// 
// ErasureCodeJerasureCauchy
//
void ErasureCodeJerasureCauchy::jerasure_encode(char **data,
						char **coding,
						int blocksize)
{
  jerasure_schedule_encode(k, m, w, schedule,
			   data, coding, blocksize, packetsize);
}

int ErasureCodeJerasureCauchy::jerasure_decode(int *erasures,
					       char **data,
					       char **coding,
					       int blocksize)
{
  return jerasure_schedule_decode_lazy(k, m, w, bitmatrix,
				       erasures, data, coding, blocksize, packetsize, 1);
}

int ErasureCodeJerasureCauchy::jerasure_decode_for_xor(int *erasures,
                 char **data,
                 char *coding,
                 int blocksize,
                 int* parity_group_selection,
                 Control* control)
{
  int r= jerasure_schedule_decode_lazy_hybrid_solution(k, m, w, bitmatrix, erasures, data, 
               coding, blocksize, packetsize, parity_group_selection, control);
  free(control);
  return r;
}

unsigned ErasureCodeJerasureCauchy::get_alignment() const
{
  if (per_chunk_alignment) {
    unsigned alignment = w * packetsize;
    unsigned modulo = alignment % LARGEST_VECTOR_WORDSIZE;
    if (modulo)
      alignment += LARGEST_VECTOR_WORDSIZE - modulo;
    return alignment;
  } else {
    unsigned alignment = k*w*packetsize*sizeof(int);
    if ( ((w*packetsize*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
      alignment = k*w*packetsize*LARGEST_VECTOR_WORDSIZE;
    return alignment;
  }  
}

int ErasureCodeJerasureCauchy::parse(ErasureCodeProfile &profile,
				     ostream *ss)
{
  int err = ErasureCodeJerasure::parse(profile, ss);
  err |= to_int("packetsize", profile, &packetsize, DEFAULT_PACKETSIZE, ss);
  err |= to_bool("jerasure-per-chunk-alignment", profile,
		 &per_chunk_alignment, "false", ss);
  return err;
}

void ErasureCodeJerasureCauchy::prepare_schedule(int *matrix)
{
  bitmatrix = jerasure_matrix_to_bitmatrix(k, m, w, matrix);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}

int* ErasureCodeJerasureCauchy::get_matrix()//add by LYF
{
  return bitmatrix;
}

int ErasureCodeJerasureCauchy::get_symbol_size()//add by LYF
{
  return packetsize;
}

// 
// ErasureCodeJerasureCauchyOrig
//
void ErasureCodeJerasureCauchyOrig::prepare()
{
  int *matrix = cauchy_original_coding_matrix(k, m, w);
  prepare_schedule(matrix);
  free(matrix);
}

// 
// ErasureCodeJerasureCauchyGood
//
void ErasureCodeJerasureCauchyGood::prepare()
{
  int *matrix = cauchy_good_general_coding_matrix(k, m, w);
  prepare_schedule(matrix);
  free(matrix);
}

// 
// ErasureCodeJerasureLiberation
//
ErasureCodeJerasureLiberation::~ErasureCodeJerasureLiberation()
{
  if (bitmatrix)
    free(bitmatrix);
  if (schedule)
    jerasure_free_schedule(schedule);
}

void ErasureCodeJerasureLiberation::jerasure_encode(char **data,
                                                    char **coding,
                                                    int blocksize)
{
  jerasure_schedule_encode(k, m, w, schedule, data,
			   coding, blocksize, packetsize);
}

int ErasureCodeJerasureLiberation::jerasure_decode(int *erasures,
                                                    char **data,
                                                    char **coding,
                                                    int blocksize)
{
  return jerasure_schedule_decode_lazy(k, m, w, bitmatrix, erasures, data,
				       coding, blocksize, packetsize, 1);
}

int ErasureCodeJerasureLiberation::jerasure_decode_for_xor(int *erasures,
                                                    char **data,
                                                    char *coding,
                                                    int blocksize,
                                                    int* parity_group_selection,
                                                    Control * control)
{
  int r = jerasure_schedule_decode_lazy_hybrid_solution(k, m, w, bitmatrix, erasures, data, 
               coding, blocksize, packetsize, parity_group_selection, control);
  free(control);
  return r;
}

unsigned ErasureCodeJerasureLiberation::get_alignment() const
{
  unsigned alignment = k*w*packetsize*sizeof(int);
  if ( ((w*packetsize*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
    alignment = k*w*packetsize*LARGEST_VECTOR_WORDSIZE;
  return alignment;
}

bool ErasureCodeJerasureLiberation::check_k(ostream *ss) const
{
  if (k > w) {
    *ss << "k=" << k << " must be less than or equal to w=" << w << std::endl;
    return false;
  } else {
    return true;
  }
}

bool ErasureCodeJerasureLiberation::check_w(ostream *ss) const
{
  if (w <= 2 || !is_prime(w)) {
    *ss <<  "w=" << w << " must be greater than two and be prime" << std::endl;
    return false;
  } else {
    return true;
  }
}

bool ErasureCodeJerasureLiberation::check_packetsize_set(ostream *ss) const
{
  if (packetsize == 0) {
    *ss << "packetsize=" << packetsize << " must be set" << std::endl;
    return false;
  } else {
    return true;
  }
}

bool ErasureCodeJerasureLiberation::check_packetsize(ostream *ss) const
{
  if ((packetsize%(sizeof(int))) != 0) {
    *ss << "packetsize=" << packetsize
	<< " must be a multiple of sizeof(int) = " << sizeof(int) << std::endl;
    return false;
  } else {
    return true;
  }
}

int ErasureCodeJerasureLiberation::revert_to_default(ErasureCodeProfile &profile,
						     ostream *ss)
{
  int err = 0;
  *ss << "reverting to k=" << DEFAULT_K << ", w="
      << DEFAULT_W << ", packetsize=" << DEFAULT_PACKETSIZE << std::endl;
  profile["k"] = DEFAULT_K;
  err |= to_int("k", profile, &k, DEFAULT_K, ss);
  profile["w"] = DEFAULT_W;
  err |= to_int("w", profile, &w, DEFAULT_W, ss);
  profile["packetsize"] = DEFAULT_PACKETSIZE;
  err |= to_int("packetsize", profile, &packetsize, DEFAULT_PACKETSIZE, ss);
  return err;
}

int ErasureCodeJerasureLiberation::parse(ErasureCodeProfile &profile,
					 ostream *ss)
{
  int err = ErasureCodeJerasure::parse(profile, ss);
  err |= to_int("packetsize", profile, &packetsize, DEFAULT_PACKETSIZE, ss);

  bool error = false;
  if (!check_k(ss))
    error = true;
  if (!check_w(ss))
    error = true;
  if (!check_packetsize_set(ss) || !check_packetsize(ss))
    error = true;
  if (error) {
    revert_to_default(profile, ss);
    err = -EINVAL;
  }
  return err;
}

void ErasureCodeJerasureLiberation::prepare()
{
  bitmatrix = liberation_coding_bitmatrix(k, w);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}

int* ErasureCodeJerasureLiberation::get_matrix()//add by LYF
{
  return bitmatrix;
}

int ErasureCodeJerasureLiberation::get_symbol_size()//add bu LYF
{
  return packetsize;
}

// 
// ErasureCodeJerasureBlaumRoth
//
bool ErasureCodeJerasureBlaumRoth::check_w(ostream *ss) const
{
  // back in Firefly, w = 7 was the default and produced useable 
  // chunks. Tolerate this value for backward compatibility.
  if (w == 7)
    return true;
  if (w <= 2 || !is_prime(w+1)) {
    *ss <<  "w=" << w << " must be greater than two and "
	<< "w+1 must be prime" << std::endl;
    return false;
  } else {
    return true;
  }
}

void ErasureCodeJerasureBlaumRoth::prepare()
{
  bitmatrix = blaum_roth_coding_bitmatrix(k, w);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}

// 
// ErasureCodeJerasureLiber8tion
//
int ErasureCodeJerasureLiber8tion::parse(ErasureCodeProfile &profile,
					 ostream *ss)
{
  int err = ErasureCodeJerasure::parse(profile, ss);
  profile.erase("m");
  err |= to_int("m", profile, &m, DEFAULT_M, ss);
  profile.erase("w");
  err |= to_int("w", profile, &w, DEFAULT_W, ss);
  err |= to_int("packetsize", profile, &packetsize, DEFAULT_PACKETSIZE, ss);

  bool error = false;
  if (!check_k(ss))
    error = true;
  if (!check_packetsize_set(ss))
    error = true;
  if (error) {
    revert_to_default(profile, ss);
    err = -EINVAL;
  }
  return err;
}

void ErasureCodeJerasureLiber8tion::prepare()
{
  bitmatrix = liber8tion_coding_bitmatrix(k);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}
