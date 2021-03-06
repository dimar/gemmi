// Copyright 2017 Global Phasing Ltd.

#include <stdio.h>
#include <cstdlib> // for getenv
#include <algorithm> // for count_if
#include <stdexcept>
#include "gemmi/gzread.hpp"
#include "gemmi/chemcomp.hpp"  // for ChemComp
#include "gemmi/to_cif.hpp"    // for write_cif_to_file
#include "gemmi/entstr.hpp"    // for entity_type_to_string
#include "gemmi/to_mmcif.hpp"  // for write_struct_conn
#include "gemmi/sprintf.hpp"   // for to_str, to_str_prec
#include "gemmi/calculate.hpp" // for find_best_plane
#include "gemmi/polyheur.hpp"  // for remove_hydrogens
#include "gemmi/monlib.hpp"    // for MonLib, read_monomers
#include "gemmi/topo.hpp"      // for Topo

#define GEMMI_PROG crdrst
#include "options.h"

namespace cif = gemmi::cif;
using gemmi::Topo;

enum OptionIndex { Verbose=3, Monomers, NoHydrogens, KeepHydrogens,
                   NoZeroOccRestr };

static const option::Descriptor Usage[] = {
  { NoOp, 0, "", "", Arg::None,
    "Usage:"
    "\n " EXE_NAME " [options] INPUT_FILE OUTPUT_BASENAME"
    "\n\nMake intermediate files from one of PDB, mmCIF or mmJSON formats."
    "\n\nOptions:" },
  { Help, 0, "h", "help", Arg::None, "  -h, --help  \tPrint usage and exit." },
  { Version, 0, "V", "version", Arg::None,
    "  -V, --version  \tPrint version and exit." },
  { Verbose, 0, "", "verbose", Arg::None, "  --verbose  \tVerbose output." },
  { Monomers, 0, "", "monomers", Arg::Required,
    "  --monomers=DIR  \tMonomer library dir (default: $CLIBD_MON)." },
  { NoHydrogens, 0, "H", "no-hydrogens", Arg::None,
    "  -H, --no-hydrogens  \tRemove or do not add hydrogens." },
  { KeepHydrogens, 0, "", "keep-hydrogens", Arg::None,
    "  --keep-hydrogens  \tPreserve hydrogens from the input file." },
  { NoZeroOccRestr, 0, "", "no-zero-occ", Arg::None,
    "  --no-zero-occ  \tNo restraints for zero-occupancy atoms." },
  { 0, 0, 0, 0, 0, 0 }
};


// Topology: restraints applied to a model
static int count_provenance(const std::vector<Topo::Force>& forces,
                            Topo::Provenance p) {
  return std::count_if(forces.begin(), forces.end(),
                       [&](const Topo::Force& f) { return f.provenance == p; });
}

static bool has_anisou(const gemmi::Model& model) {
  for (const gemmi::Chain& chain : model.chains)
    for (const gemmi::Residue& res : chain.residues)
      for (const gemmi::Atom& a : res.atoms)
        if (a.has_anisou())
          return true;
  return false;
}

// for compatibility with makecif, not sure what ccp4_mod_id is really used for
static std::string get_ccp4_mod_id(const std::vector<std::string>& mods) {
  for (const std::string& m : mods)
    if (m != "AA-STAND" && !gemmi::starts_with(m, "DEL-OXT") &&
        !gemmi::starts_with(m, "DEL-HN") && m != "DEL-NMH")
      return m;
  return ".";
}

static cif::Document make_crd(const gemmi::Structure& st,
                              const gemmi::MonLib& monlib,
                              const Topo& topo) {
  using gemmi::to_str;
  cif::Document crd;
  auto e_id = st.info.find("_entry.id");
  std::string id = (e_id != st.info.end() ? e_id->second : st.name);
  crd.blocks.emplace_back("structure_" + id);
  cif::Block& block = crd.blocks[0];
  auto& items = block.items;

  items.emplace_back("_entry.id", id);
  items.emplace_back("_database_2.code_PDB", id);
  auto keywords = st.info.find("_struct_keywords.pdbx_keywords");
  if (keywords != st.info.end())
    items.emplace_back("_struct_keywords.text", cif::quote(keywords->second));
  auto title = st.info.find("_struct.title");
  if (title != st.info.end())
    items.emplace_back(title->first, cif::quote(title->second));
  auto initial_date =
         st.info.find("_pdbx_database_status.recvd_initial_deposition_date");
  if (initial_date != st.info.end())
    items.emplace_back("_audit.creation_date", initial_date->second);
  items.emplace_back("_software.name", "gemmi");

  items.emplace_back(cif::CommentArg{"############\n"
                                     "## ENTITY ##\n"
                                     "############"});
  cif::Loop& entity_loop = block.init_mmcif_loop("_entity.", {"id", "type"});
  for (const gemmi::Entity& ent : st.entities)
    entity_loop.add_row({ent.name, entity_type_to_string(ent.entity_type)});
  items.emplace_back(cif::CommentArg{"#####################\n"
                                     "## ENTITY_POLY_SEQ ##\n"
                                     "#####################"});
  cif::Loop& poly_loop = block.init_mmcif_loop("_entity_poly_seq.", {
              "mon_id", "ccp4_auth_seq_id", "entity_id",
              "ccp4_back_connect_type", "ccp4_num_mon_back", "ccp4_mod_id"});
  for (const Topo::ChainInfo& chain_info : topo.chains) {
    if (!chain_info.polymer)
      continue;
    for (const Topo::ResInfo& res_info : chain_info.residues) {
      std::string prev = res_info.prev_idx ? res_info.prev_res()->seqid.str()
                                           : "n/a";
      std::string mod = get_ccp4_mod_id(res_info.mods);
      poly_loop.add_row({res_info.res->name, res_info.res->seqid.str(),
                         chain_info.entity_id, res_info.prev_link, prev, mod});
    }
  }
  items.emplace_back(cif::CommentArg{"##########\n"
                                     "## CELL ##\n"
                                     "##########"});
  items.emplace_back("_cell.entry_id", id);
  items.emplace_back("_cell.length_a",    to_str(st.cell.a));
  items.emplace_back("_cell.length_b",    to_str(st.cell.b));
  items.emplace_back("_cell.length_c",    to_str(st.cell.c));
  items.emplace_back("_cell.angle_alpha", to_str(st.cell.alpha));
  items.emplace_back("_cell.angle_beta",  to_str(st.cell.beta));
  items.emplace_back("_cell.angle_gamma", to_str(st.cell.gamma));
  bool write_fract_matrix = true;
  if (write_fract_matrix) {
    items.emplace_back(cif::CommentArg{"##############################\n"
                                       "## FRACTIONALISATION MATRIX ##\n"
                                       "##############################"});
    std::string prefix = "_atom_sites.fract_transf_";
    for (int i = 0; i < 3; ++i) {
      std::string start = prefix + "matrix[" + std::to_string(i+1) + "][";
      const auto& row = st.cell.frac.mat[i];
      for (int j = 0; j < 3; ++j)
        items.emplace_back(start + std::to_string(j+1) + "]", to_str(row[j]));
    }
    for (int i = 0; i < 3; ++i)
      items.emplace_back(prefix + "vector[" + std::to_string(i + 1) + "]",
                         to_str(st.cell.frac.vec.at(i)));
  }

  items.emplace_back(cif::CommentArg{"##############\n"
                                     "## SYMMETRY ##\n"
                                     "##############"});
  items.emplace_back("_symmetry.entry_id", id);
  const std::string& hm = st.spacegroup_hm;
  items.emplace_back("_symmetry.space_group_name_H-M", cif::quote(hm));
  if (const gemmi::SpaceGroup* sg = gemmi::find_spacegroup_by_name(hm))
    items.emplace_back("_symmetry.Int_Tables_number",
                       std::to_string(sg->number));
  const gemmi::Model& model0 = st.models.at(0);
  items.emplace_back(cif::CommentArg{"#################\n"
                                     "## STRUCT_ASYM ##\n"
                                     "#################"});
  cif::Loop& asym_loop = block.init_mmcif_loop("_struct_asym.",
                                               {"id", "entity_id"});
  for (const gemmi::Chain& chain : model0.chains)
    for (gemmi::SubChain sub : const_cast<gemmi::Chain&>(chain).subchains())
      if (sub.labelled()) {
        const gemmi::Entity* ent = st.get_entity_of(sub);
        asym_loop.add_row({sub.name(), (ent ? ent->name : "?")});
      }
  const auto& connections = model0.connections;
  if (!connections.empty()) {
    items.emplace_back(cif::CommentArg{"#################\n"
                                       "## STRUCT_CONN ##\n"
                                       "#################"});
    gemmi::impl::write_struct_conn(st, block);
  }

  items.emplace_back(cif::CommentArg{"###############\n"
                                     "## ATOM_SITE ##\n"
                                     "###############"});
  cif::Loop& atom_loop = block.init_mmcif_loop("_atom_site.", {
      "group_PDB",
      "id",
      "label_atom_id",
      "label_alt_id",
      "label_comp_id",
      "label_asym_id",
      "auth_seq_id",
      //"pdbx_PDB_ins_code",
      "Cartn_x",
      "Cartn_y",
      "Cartn_z",
      "occupancy",
      "B_iso_or_equiv",
      "type_symbol",
      "calc_flag",
      "label_seg_id",
      "auth_atom_id",
      "label_chem_id"});
  bool write_anisou = has_anisou(model0);
  if (write_anisou)
    for (const char* idx : {"[1][1]", "[2][2]", "[3][3]",
                            "[1][2]", "[1][3]", "[2][3]"})
      atom_loop.tags.push_back(std::string("_atom_site.aniso_U") + idx);
  std::vector<std::string>& vv = atom_loop.values;
  vv.reserve(count_atom_sites(st) * atom_loop.tags.size());
  for (const gemmi::Chain& chain : model0.chains) {
    for (const gemmi::Residue& res : chain.residues) {
      std::string auth_seq_id = res.seqid.num.str();
      //std::string ins_code(1, res.icode != ' ' ? res.icode : '?');
      const gemmi::ChemComp& cc = monlib.monomers.at(res.name);
      for (const gemmi::Atom& a : res.atoms) {
        vv.emplace_back("ATOM");
        vv.emplace_back(std::to_string(a.serial));
        vv.emplace_back(a.name);
        vv.emplace_back(1, a.altloc ? a.altloc : '.');
        vv.emplace_back(res.name);
        vv.emplace_back(chain.name);
        vv.emplace_back(auth_seq_id);
        //vv.emplace_back(ins_code);
        vv.emplace_back(to_str(a.pos.x));
        vv.emplace_back(to_str(a.pos.y));
        vv.emplace_back(to_str(a.pos.z));
        vv.emplace_back(to_str(a.occ));
        vv.emplace_back(to_str(a.b_iso));
        vv.emplace_back(a.element.uname());
        vv.emplace_back(1, a.flag ? a.flag : '.'); // calc_flag
        vv.emplace_back(1, '.'); // label_seg_id
        vv.emplace_back(a.name); // again
        vv.emplace_back(cc.get_atom(a.name).chem_type); // label_chem_id
        if (write_anisou) {
          if (a.has_anisou()) {
            for (float u : {a.u11, a.u22, a.u33, a.u12, a.u13, a.u23})
              vv.push_back(to_str(u));
          } else {
            vv.resize(vv.size() + 6, ".");
          }
        }
      }
    }
  }
  return crd;
}

static void add_restraints(const Topo::Force force,
                           const Topo& topo, const gemmi::Restraints& rt,
                           cif::Loop& restr_loop, int (&counters)[5]) {
  //using gemmi::to_str;
  const auto& to_str = gemmi::to_str_prec<3>; // to make comparisons easier
  const auto& to_str3 = gemmi::to_str_prec<3>;
  if (force.rkind == Topo::RKind::Bond) {
    const Topo::Bond& t = topo.bonds[force.index];
    std::string obs = to_str3(t.calculate()) +
                      " # " + t.atoms[0]->name + " " + t.atoms[1]->name;
    restr_loop.add_row({"BOND", std::to_string(++counters[0]),
                        bond_type_to_string(t.restr->type), ".",
                        std::to_string(t.atoms[0]->serial),
                        std::to_string(t.atoms[1]->serial),
                        ".", ".",
                        to_str(t.restr->value), to_str(t.restr->esd), obs});
  } else if (force.rkind == Topo::RKind::Angle) {
    const Topo::Angle& t = topo.angles[force.index];
    std::string obs = to_str3(gemmi::deg(t.calculate()));
    obs += " # " + t.atoms[0]->name + " " +
                   t.atoms[1]->name + " " +
                   t.atoms[2]->name;
    restr_loop.add_row({"ANGL", std::to_string(++counters[1]),
                        ".", ".",
                        std::to_string(t.atoms[0]->serial),
                        std::to_string(t.atoms[1]->serial),
                        std::to_string(t.atoms[2]->serial),
                        ".",
                        to_str(t.restr->value), to_str(t.restr->esd), obs});
  } else if (force.rkind == Topo::RKind::Torsion) {
    const Topo::Torsion& t = topo.torsions[force.index];
    std::string obs = to_str3(gemmi::deg(t.calculate()));
    obs += " # " + t.atoms[0]->name + " " + t.atoms[1]->name +
           " " + t.atoms[2]->name + " " + t.atoms[3]->name;
    restr_loop.add_row({"TORS", std::to_string(++counters[2]),
                        t.restr->label, std::to_string(t.restr->period),
                        std::to_string(t.atoms[0]->serial),
                        std::to_string(t.atoms[1]->serial),
                        std::to_string(t.atoms[2]->serial),
                        std::to_string(t.atoms[3]->serial),
                        to_str(t.restr->value), to_str(t.restr->esd), obs});
  } else if (force.rkind == Topo::RKind::Chirality) {
    const Topo::Chirality& t = topo.chirs[force.index];
    double vol = rt.chiral_abs_volume(*t.restr);
    std::string obs = to_str3(t.calculate()) + " # " + t.atoms[0]->name +
                                                 " " + t.atoms[1]->name +
                                                 " " + t.atoms[2]->name +
                                                 " " + t.atoms[3]->name;
    restr_loop.add_row({"CHIR", std::to_string(++counters[3]),
                        gemmi::chirality_to_string(t.restr->chir), ".",
                        std::to_string(t.atoms[0]->serial),
                        std::to_string(t.atoms[1]->serial),
                        std::to_string(t.atoms[2]->serial),
                        std::to_string(t.atoms[3]->serial),
                        to_str3(vol), "0.020", obs});
  } else if (force.rkind == Topo::RKind::Plane) {
    const Topo::Plane& t = topo.planes[force.index];
    ++counters[4];
    auto coeff = find_best_plane(t.atoms);
    for (const gemmi::Atom* atom : t.atoms) {
      double dist = gemmi::get_distance_from_plane(atom->pos, coeff);
      std::string obs = to_str3(dist) + " # " + atom->name;
      restr_loop.add_row({"PLAN", std::to_string(counters[4]), t.restr->label,
                          ".", std::to_string(atom->serial), ".", ".", ".",
                          to_str(t.restr->esd), ".", obs});
    }
  }
}

static cif::Document make_rst(const Topo& topo, const gemmi::MonLib& monlib) {
  using Provenance = Topo::Provenance;
  cif::Document doc;
  doc.blocks.emplace_back("restraints");
  cif::Block& block = doc.blocks[0];
  cif::Loop& restr_loop = block.init_mmcif_loop("_restr.", {
              "record", "number", "label", "period",
              "atom_id_1", "atom_id_2", "atom_id_3", "atom_id_4",
              "value", "dev", "val_obs"});
  int counters[5] = {0, 0, 0, 0, 0};
  for (const Topo::ChainInfo& chain_info : topo.chains) {
    for (const Topo::ResInfo& ri : chain_info.residues) {
      // write link
      if (const gemmi::Residue* prev = ri.prev_res()) {
        const gemmi::ChemLink* link = monlib.find_link(ri.prev_link);
        if (link && count_provenance(ri.forces, Provenance::PrevLink) > 0) {
          std::string comment = " link " + ri.prev_link + " " +
                                 prev->seqid.str() + " " + prev->name + " - " +
                                 ri.res->seqid.str() + " " + ri.res->name;
          restr_loop.add_comment_and_row({comment, "LINK", ".",
                                          cif::quote(ri.prev_link), ".",
                                          ".", ".", ".", ".", ".", ".", "."});
          for (const Topo::Force& force : ri.forces)
            if (force.provenance == Provenance::PrevLink)
              add_restraints(force, topo, link->rt, restr_loop, counters);
        }
      }
      // write monomer
      if (count_provenance(ri.forces, Provenance::Monomer) > 0) {
        std::string res_info = " monomer " + chain_info.name + " " +
                               ri.res->seqid.str() + " " + ri.res->name;

        // need to revisit it later on
        std::string group = cif::quote(ri.chemcomp.group.substr(0, 8));
        if (group == "peptide" || group == "P-peptid" || group == "M-peptid")
          group = "L-peptid";

        restr_loop.add_comment_and_row({res_info, "MONO", ".", group, ".",
                                        ".", ".", ".", ".", ".", ".", "."});
        for (const Topo::Force& force : ri.forces)
          if (force.provenance == Provenance::Monomer)
            add_restraints(force, topo, ri.chemcomp.rt, restr_loop, counters);
      }
    }
  }
  // explicit links
  for (const Topo::ExtraLink& link : topo.extras) {
    const gemmi::ChemLink* chem_link = monlib.match_link(link.link);
    assert(chem_link);
    std::string comment = " link " + chem_link->id;
    restr_loop.add_comment_and_row({comment, "LINK", ".",
                                    cif::quote(chem_link->id), ".",
                                    ".", ".", ".", ".", ".", ".", "."});
    for (const Topo::Force& force : link.forces)
      add_restraints(force, topo, chem_link->rt, restr_loop, counters);
  }
  return doc;
}


/*
static void move_hydrogen_1_1(const gemmi::Atom* a1,
                              const gemmi::Atom* a2,
                              const gemmi::Atom* a3,
                              gemmi::Atom* a4,
                              double dist,
                              double theta,
                              double tau) {
}
*/

static void place_hydrogens(const gemmi::Atom& atom, Topo::ResInfo& ri,
                            const Topo& topo) {
  std::vector<gemmi::Atom*> bonded_h;
  std::vector<gemmi::Atom*> bonded_non_h;
  for (const Topo::Force& force : ri.forces)
    if (force.rkind == Topo::RKind::Bond) {
      const Topo::Bond& t = topo.bonds[force.index];
      int n = Topo::has_atom(&atom, t);
      if (n == 0 || n == 1) {
        gemmi::Atom* other = t.atoms[1-n];
        (other->is_hydrogen() ? bonded_h : bonded_non_h).push_back(other);
      }
    }
  //printf("%s: %zd %zd\n", atom.name.c_str(), bonded_h.size(), bonded_non_h.size());
  if (bonded_h.size() == 1 && bonded_non_h.size() == 1) {
    gemmi::Atom* h = bonded_h[0];
    const gemmi::Restraints::Bond* bond = topo.take_bond(h, &atom);
    const gemmi::Restraints::Angle* angle = topo.take_angle(h, &atom,
                                                            bonded_non_h[0]);
    if (!bond || !angle)
      return;
    double h_dist = bond->value;
    if (angle->value == 180.0) {
      // easy
    } else {
      // TODO: plane to dihedral angle 0
      // using one dihedral angle
      double theta = gemmi::rad(angle->value);
      for (const Topo::Torsion& tor : topo.torsions)
        if (tor.atoms[0] == h && !tor.atoms[3]->is_hydrogen()) {
          using gemmi::Vec3;
          assert(tor.atoms[1] == &atom);
          assert(tor.atoms[2] == bonded_non_h[0]);
          const Vec3& x1 = tor.atoms[3]->pos;
          const Vec3& x2 = tor.atoms[2]->pos;
          const Vec3& x3 = atom.pos;
          Vec3& x4 = h->pos;
          double tau = gemmi::rad(tor.restr->value);
          Vec3 u = x2 - x1;
          Vec3 v = x3 - x2;
          Vec3 e1 = v.normalized();
          double delta = u.dot(e1);
          Vec3 e2 = -(u - delta * e1).normalized();
          Vec3 e3 = e1.cross(e2);
          x4 = x3 + h_dist * (-cos(theta) * e1 +
                              sin(theta) * cos(tau) * e2 +
                              sin(theta) * sin(tau) * e3);
          // TODO
        } else if (tor.atoms[3] == h && !tor.atoms[0]->is_hydrogen()) {
          // TODO
        }
    }
  }
  // TODO
}

int GEMMI_MAIN(int argc, char **argv) {
  OptParser p(EXE_NAME);
  p.simple_parse(argc, argv, Usage);
  p.require_positional_args(2);
  const char* monomer_dir = p.options[Monomers] ? p.options[Monomers].arg
                                                : std::getenv("CLIBD_MON");
  if (monomer_dir == nullptr || *monomer_dir == '\0') {
    fprintf(stderr, "Set $CLIBD_MON or use option --monomers.\n");
    return 1;
  }
  std::string input = p.coordinate_input_file(0);
  std::string output = p.nonOption(1);
  if (p.options[KeepHydrogens] && p.options[NoHydrogens])
    gemmi::fail("cannot use both --no-hydrogens and --keep-hydrogens");
  try {
    gemmi::Structure st = gemmi::read_structure_gz(input);
    if (st.input_format == gemmi::CoorFormat::Pdb)
      gemmi::setup_entities(st);
    if (st.models.empty())
      return 1;
    gemmi::Model& model0 = st.models[0];
    if (!p.options[KeepHydrogens])
      gemmi::remove_hydrogens(model0);

    gemmi::MonLib monlib = gemmi::read_monomers(monomer_dir, model0,
                                                gemmi::read_cif_gz);

    // add H, sort atoms in residues and assign serial numbers
    int serial = 0;
    for (gemmi::Chain& chain : model0.chains)
      for (gemmi::Residue& res : chain.residues) {
        const gemmi::ChemComp &cc = monlib.monomers.at(res.name);
        for (gemmi::Atom& atom : res.atoms) {
          auto it = cc.find_atom(atom.name);
          if (it == cc.atoms.end())
            gemmi::fail("No atom " + atom.name + " expected in " + res.name);
          atom.serial = it - cc.atoms.begin();
        }
        if (!p.options[KeepHydrogens] && !p.options[NoHydrogens])
          for (auto it = cc.atoms.begin(); it != cc.atoms.end(); ++it)
            if (is_hydrogen(it->el)) {
              gemmi::Atom atom = it->to_full_atom();
              atom.flag = 'R';
              atom.serial = it - cc.atoms.begin();
              res.atoms.push_back(atom);
            }
        std::sort(res.atoms.begin(), res.atoms.end(),
                  [](const gemmi::Atom& a, const gemmi::Atom& b) {
                    return a.serial != b.serial ? a.serial < b.serial
                                                : a.altloc < b.altloc;
                  });
        for (gemmi::Atom& atom : res.atoms)
          atom.serial = ++serial;
      }

    Topo topo;
    topo.prepare_refmac_topology(model0, st.entities, monlib);

    if (!p.options[KeepHydrogens] && !p.options[NoHydrogens])
      for (Topo::ChainInfo& chain_info : topo.chains)
        for (Topo::ResInfo& ri : chain_info.residues)
          for (gemmi::Atom& atom : ri.res->atoms)
            if (!atom.is_hydrogen())
              place_hydrogens(atom, ri, topo);

    cif::Document crd = make_crd(st, monlib, topo);
    if (p.options[Verbose])
      printf("Writing coordinates to: %s.crd\n", output.c_str());
    write_cif_to_file(crd, output + ".crd", cif::Style::NoBlankLines);

    if (p.options[NoZeroOccRestr])
      for (gemmi::Chain& chain : model0.chains)
        for (gemmi::Residue& res : chain.residues)
          for (gemmi::Atom& atom : res.atoms)
            if (atom.occ <= 0) {
              if (p.options[Verbose])
                printf("Atom with zero occupancy: %s\n",
                       gemmi::atom_str(chain, res, atom).c_str());
              atom.name += '?';  // hide the atom by mangling the name
            }

    cif::Document rst = make_rst(topo, monlib);
    if (p.options[Verbose])
      printf("Writing restraints to: %s.rst\n", output.c_str());
    write_cif_to_file(rst, output + ".rst", cif::Style::NoBlankLines);
  } catch (std::runtime_error& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    return 1;
  } catch (std::out_of_range& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    return 1;
  }
  return 0;
}

// vim:sw=2:ts=2:et:path^=../include,../third_party
