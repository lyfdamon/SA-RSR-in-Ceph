// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2013, 2014 Cloudwatt <libre.licensing@cloudwatt.com>
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

#ifndef CEPH_ERASURE_CODE_JERASURE_H
#define CEPH_ERASURE_CODE_JERASURE_H

#include "erasure-code/ErasureCode.h"
extern "C" {
#include "control.h"
}
#define DEFAULT_RULESET_ROOT "default"
#define DEFAULT_RULESET_FAILURE_DOMAIN "host"

class ErasureCodeJerasure : public ErasureCode {
public:
  int k;
  std::string DEFAULT_K;
  int m;
  std::string DEFAULT_M;
  int w;
  std::string DEFAULT_W;
  const char *technique;
  string ruleset_root;
  string ruleset_failure_domain;
  bool per_chunk_alignment;

  explicit ErasureCodeJerasure(const char *_technique) :
    k(0),
    DEFAULT_K("2"),
    m(0),
    DEFAULT_M("1"),
    w(0),
    DEFAULT_W("8"),
    technique(_technique),
    ruleset_root(DEFAULT_RULESET_ROOT),
    ruleset_failure_domain(DEFAULT_RULESET_FAILURE_DOMAIN),
    per_chunk_alignment(false)
  {}

  ~ErasureCodeJerasure() override {}
  
  int create_ruleset(const string &name,
			     CrushWrapper &crush,
			     ostream *ss) const override;

  unsigned int get_chunk_count() const override {
    return k + m;
  }

  unsigned int get_data_chunk_count() const override {
    return k;
  }

  unsigned int get_symbol_count() const override {//add by LYF
    return w;
  }

  unsigned int get_chunk_size(unsigned int object_size) const override;

  int encode_chunks(const set<int> &want_to_encode,
			    map<int, bufferlist> *encoded) override;

  int decode_chunks(const set<int> &want_to_read,
			    const map<int, bufferlist> &chunks,
			    map<int, bufferlist> *decoded) override;

  int decode_chunks_for_xor(const set<int> &want_to_read,
          const map<int, bufferlist> &chunks,
          map<int, bufferlist> *decoded,
          unsigned blocksize,
          int packet_size, 
          int w, 
          map<int,vector<int> > solution,
          int* parity_group_selection) override;//add by LYF

  void get_Control(int k, int w, map<int, vector<int> > solution, Control* control);//add by LYF

  int init(ErasureCodeProfile &profile, ostream *ss) override;

  int* get_bitmatrix() override;//add by LYF
  int get_packetsize() override;//add by LYF

  virtual void jerasure_encode(char **data,
                               char **coding,
                               int blocksize) = 0;
  virtual int jerasure_decode(int *erasures,
                               char **data,
                               char **coding,
                               int blocksize) = 0;
  virtual int jerasure_decode_for_xor(int *erasures, 
                               char **data, 
                               char *coding, 
                               int blocksize, 
                               int* parity_group_selection, 
                               Control* control) = 0;//add by LYF

  virtual unsigned get_alignment() const = 0;
  virtual void prepare() = 0;

  virtual int* get_matrix() = 0;//add by LYF
  virtual int get_symbol_size() = 0;//add by LYF

  static bool is_prime(int value);
protected:
  virtual int parse(ErasureCodeProfile &profile, ostream *ss);
};

class ErasureCodeJerasureReedSolomonVandermonde : public ErasureCodeJerasure {
public:
  int *matrix;

  ErasureCodeJerasureReedSolomonVandermonde() :
    ErasureCodeJerasure("reed_sol_van"),
    matrix(0)
  {
    DEFAULT_K = "7";
    DEFAULT_M = "3";
    DEFAULT_W = "8";
  }
  ~ErasureCodeJerasureReedSolomonVandermonde() override {
    if (matrix)
      free(matrix);
  }

  void jerasure_encode(char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode(int *erasures,
                               char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode_for_xor(int *erasures, 
                               char **data, 
                               char *coding, 
                               int blocksize, 
                               int* parity_group_selection, 
                               Control* control) override;//add by LYF
  unsigned get_alignment() const override;
  void prepare() override;
  int* get_matrix() override;//add by LYF
  int get_symbol_size() override;//add by LYF
private:
  int parse(ErasureCodeProfile &profile, ostream *ss) override;
};

class ErasureCodeJerasureReedSolomonRAID6 : public ErasureCodeJerasure {
public:
  int *matrix;

  ErasureCodeJerasureReedSolomonRAID6() :
    ErasureCodeJerasure("reed_sol_r6_op"),
    matrix(0)
  {
    DEFAULT_K = "7";
    DEFAULT_W = "8";
  }
  ~ErasureCodeJerasureReedSolomonRAID6() override {
    if (matrix)
      free(matrix);
  }

  void jerasure_encode(char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode(int *erasures,
                               char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode_for_xor(int *erasures, 
                               char **data, 
                               char *coding, 
                               int blocksize, 
                               int* parity_group_selection, 
                               Control* control) override;//add by LYF
  unsigned get_alignment() const override;
  void prepare() override;
  int* get_matrix() override;//add by LYF
  int get_symbol_size() override;//add by LYF
private:
  int parse(ErasureCodeProfile &profile, ostream *ss) override;
};

#define DEFAULT_PACKETSIZE "2048"

class ErasureCodeJerasureCauchy : public ErasureCodeJerasure {
public:
  int *bitmatrix;
  int **schedule;
  int packetsize;

  explicit ErasureCodeJerasureCauchy(const char *technique) :
    ErasureCodeJerasure(technique),
    bitmatrix(0),
    schedule(0),
    packetsize(0)
  {
    DEFAULT_K = "7";
    DEFAULT_M = "3";
    DEFAULT_W = "8";
  }
  ~ErasureCodeJerasureCauchy() override {
    if (bitmatrix)
      free(bitmatrix);
    if (schedule)
      free(schedule);
  }

  void jerasure_encode(char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode(int *erasures,
                               char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode_for_xor(int *erasures, 
                               char **data, 
                               char *coding, 
                               int blocksize, 
                               int* parity_group_selection, 
                               Control* control) override;//add  by LYF
  unsigned get_alignment() const override;
  void prepare_schedule(int *matrix);
  int* get_matrix() override;//add by LYF
  int get_symbol_size() override;//add by LYF
private:
  int parse(ErasureCodeProfile &profile, ostream *ss) override;
};

class ErasureCodeJerasureCauchyOrig : public ErasureCodeJerasureCauchy {
public:
  ErasureCodeJerasureCauchyOrig() :
    ErasureCodeJerasureCauchy("cauchy_orig")
  {}

  void prepare() override;
};

class ErasureCodeJerasureCauchyGood : public ErasureCodeJerasureCauchy {
public:
  ErasureCodeJerasureCauchyGood() :
    ErasureCodeJerasureCauchy("cauchy_good")
  {}

  void prepare() override;
};

class ErasureCodeJerasureLiberation : public ErasureCodeJerasure {
public:
  int *bitmatrix;
  int **schedule;
  int packetsize;

  explicit ErasureCodeJerasureLiberation(const char *technique = "liberation") :
    ErasureCodeJerasure(technique),
    bitmatrix(0),
    schedule(0),
    packetsize(0)
  {
    DEFAULT_K = "2";
    DEFAULT_M = "2";
    DEFAULT_W = "7";
  }
  ~ErasureCodeJerasureLiberation() override;

  void jerasure_encode(char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode(int *erasures,
                               char **data,
                               char **coding,
                               int blocksize) override;
  int jerasure_decode_for_xor(int *erasures, 
                               char **data, 
                               char *coding, 
                               int blocksize, 
                               int* parity_group_selection, 
                               Control* control) override;//add by LYF
  unsigned get_alignment() const override;
  virtual bool check_k(ostream *ss) const;
  virtual bool check_w(ostream *ss) const;
  virtual bool check_packetsize_set(ostream *ss) const;
  virtual bool check_packetsize(ostream *ss) const;
  virtual int revert_to_default(ErasureCodeProfile &profile,
				ostream *ss);
  void prepare() override;
  int* get_matrix() override;//add by LYF
  int get_symbol_size() override;//add by LYF
private:
  int parse(ErasureCodeProfile &profile, ostream *ss) override;
};

class ErasureCodeJerasureBlaumRoth : public ErasureCodeJerasureLiberation {
public:
  ErasureCodeJerasureBlaumRoth() :
    ErasureCodeJerasureLiberation("blaum_roth")
  {
  }

  bool check_w(ostream *ss) const override;
  void prepare() override;
};

class ErasureCodeJerasureLiber8tion : public ErasureCodeJerasureLiberation {
public:
  ErasureCodeJerasureLiber8tion() :
    ErasureCodeJerasureLiberation("liber8tion")
  {
    DEFAULT_K = "2";
    DEFAULT_M = "2";
    DEFAULT_W = "8";
  }

  void prepare() override;
private:
  int parse(ErasureCodeProfile &profile, ostream *ss) override;
};

#endif
