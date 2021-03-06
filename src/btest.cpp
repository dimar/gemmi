// Copyright 2018 Global Phasing Ltd.
//
// B-factor model testing

// TODO: calculation in asu only

#include <gemmi/subcells.hpp>
#include <gemmi/elem.hpp>  // for is_hydrogen
#include <gemmi/math.hpp>  // for Correlation
#include <gemmi/resinfo.hpp>  // for find_tabulated_residue
#include <gemmi/gzread.hpp>
#define GEMMI_PROG btest
#include "options.h"
#include <stdio.h>
#include <cstdlib>  // for strtod
#include <algorithm>  // for sort

using namespace gemmi;

enum OptionIndex { Verbose=3, FromFile, ListResidues,
                   MinDist, MaxDist, Exponent };

static const option::Descriptor Usage[] = {
  { NoOp, 0, "", "", Arg::None,
    "Usage:\n " EXE_NAME " [options] INPUT[...]"
    "\nB-factor model testing."},
  { Help, 0, "h", "help", Arg::None, "  -h, --help  \tPrint usage and exit." },
  { Version, 0, "V", "version", Arg::None,
    "  -V, --version  \tPrint version and exit." },
  { Verbose, 0, "v", "verbose", Arg::None, "  --verbose  \tVerbose output." },
  { FromFile, 0, "f", "file", Arg::Required,
    "  -f, --file=FILE  \tobtain paths or PDB IDs from FILE, one per line" },
  { ListResidues, 0, "l", "list", Arg::None,
    "  -l, --list  \tList per-residue values." },
  { MinDist, 0, "", "min-dist", Arg::Float,
    "  --min-dist=DIST  \tMinimum distance for \"contacts\" (default: 0.8)." },
  { MaxDist, 0, "", "cutoff", Arg::Float,
    "  --cutoff=DIST  \tMaximum distance for \"contacts\" (default: 15)." },
  { Exponent, 0, "", "pow", Arg::Float,
    "  --pow=P  \tExponent in the weighting (default: 2)." },
  { 0, 0, 0, 0, 0, 0 }
};

struct Params {
  float min_dist = 0.8f;
  float max_dist = 15.0f;
  float exponent = 2.0f;
};

// ranks are from 1 to data.size()
static std::vector<int> get_ranks(const std::vector<double>& data) {
  std::vector<int> indices(data.size());
  for (size_t i = 0; i != indices.size(); ++i)
    indices[i] = (int) i;
  std::sort(indices.begin(), indices.end(),
            [&data](int a, int b) { return data[a] < data[b]; });
  std::vector<int> result(data.size());
  for (size_t i = 0; i < indices.size(); ++i)
    result[indices[i]] = (int) i + 1;
  return result;
}

template<typename T>
Correlation calculate_correlation(const std::vector<T>& a,
                                  const std::vector<T>& b) {
  assert(a.size() == b.size());
  Correlation cc;
  for (size_t i = 0; i != a.size(); ++i)
    cc.add_point(a[i], b[i]);
  return cc;
}

void normalize(std::vector<double>& values) {
  Variance variance;
  for (double x : values)
    variance.add_point(x);
  double stddev = std::sqrt(variance.for_population());
  for (double& x : values)
    x = (x - variance.mean_x) / stddev;
}

struct Result {
  int n;
  double b_mean;
  double cc;
  double rank_cc;
};

static float calculate_weight(float dist_sq, const Params& params) {
  if (params.exponent == 2.0)  // canonical WCN
    return 1.0f / dist_sq;
  if (params.exponent == 0.0) // CN (a.k.a ACN)
    return 1.0f;
  return std::pow(dist_sq, -0.5f * params.exponent);
}

static Result test_bfactor_models(const Structure& st, const Params& params) {
  SubCells sc(st.models.at(0), st.cell, params.max_dist);
  const Model& model = st.models.at(0);
  std::vector<double> b_exper;
  std::vector<double> b_predict;
  for (const Chain& chain : model.chains) {
    for (const Residue& res : chain.residues) {
      if (!find_tabulated_residue(res.name).is_amino_acid())
        continue;
      for (const Atom& atom : res.atoms) {
        if (is_hydrogen(atom.element))
          continue;
        double wcn = 0;
        sc.for_each(atom.pos, atom.altloc, params.max_dist,
                    [&](const SubCells::Mark& m, float dist_sq) {
            if (dist_sq > sq(params.min_dist) && !is_hydrogen(m.element)) {
              const_CRA cra = m.to_cra(model);
              ResidueInfo res_inf = find_tabulated_residue(cra.residue->name);
              if (res_inf.is_amino_acid())
                wcn += calculate_weight(dist_sq, params) * cra.atom->occ;
            }
        });
        if (wcn == 0.0)
          continue;
        b_exper.push_back(atom.b_iso);
        b_predict.push_back(1 / wcn);
      }
    }
  }
  //normalize(b_predict);
  Correlation cc = calculate_correlation(b_exper, b_predict);
  Correlation rank_cc = calculate_correlation(get_ranks(b_exper),
                                              get_ranks(b_predict));
  Result r;
  r.b_mean = cc.mean_x;
  r.n = b_exper.size();
  r.cc = cc.coefficient();
  r.rank_cc = rank_cc.coefficient();
  return r;
}

int GEMMI_MAIN(int argc, char **argv) {
  OptParser p(EXE_NAME);
  p.simple_parse(argc, argv, Usage);

  std::vector<std::string> paths = p.paths_from_args_or_file(FromFile, 0, true);
  bool verbose = p.options[Verbose].count();
  Params params;
  if (p.options[MinDist])
    params.min_dist = std::strtof(p.options[MinDist].arg, nullptr);
  if (p.options[MaxDist])
    params.max_dist = std::strtof(p.options[MaxDist].arg, nullptr);
  if (p.options[Exponent])
    params.exponent = std::strtof(p.options[Exponent].arg, nullptr);
  double sum_cc = 0;
  double sum_rank_cc = 0;
  try {
    for (const std::string& path : paths) {
      if (verbose > 0)
        std::printf("File: %s\n", path.c_str());
      Structure st = read_structure_gz(path);
      Result r = test_bfactor_models(st, params);
      printf("%s <B>=%#.4g for %5d atoms   CC=%#.4g  rankCC=%#.4g\n",
             st.name.c_str(), r.b_mean, r.n, r.cc, r.rank_cc);
      sum_cc += r.cc;
      sum_rank_cc += r.rank_cc;
    }
    if (paths.size() > 1)
      printf("average of %4zu files             CC=%#.4g  rankCC=%#.4g\n",
             paths.size(), sum_cc / paths.size(), sum_rank_cc / paths.size());
  } catch (std::runtime_error& e) {
    std::fprintf(stderr, "ERROR: %s\n", e.what());
    return 1;
  }
  return 0;
}

// vim:sw=2:ts=2:et:path^=../include,../third_party
