// Copyright 2017-2018 Global Phasing Ltd.
//
// Heuristic methods for working with chains and polymers.
// Includes also a few well-defined functions, such as removal of hydrogens.

#ifndef GEMMI_POLYHEUR_HPP_
#define GEMMI_POLYHEUR_HPP_

#include <vector>
#include "model.hpp"
#include "resinfo.hpp"  // for find_tabulated_residue
#include "util.hpp"     // for vector_remove_if

namespace gemmi {

// A simplistic classification. It may change in the future.
// It returns PolymerType which corresponds to _entity_poly.type,
// but here we use only PeptideL, Rna, Dna, DnaRnaHybrid and Unknown.
inline PolymerType check_polymer_type(const SubChain& polymer) {
  if (polymer.size() < 2)
    return PolymerType::Unknown;
  size_t counts[9] = {0};
  size_t aa = 0;
  size_t na = 0;
  for (const Residue& r : polymer)
    if (r.entity_type == EntityType::Unknown ||
        r.entity_type == EntityType::Polymer) {
      ResidueInfo info = find_tabulated_residue(r.name);
      if (info.found())
        counts[info.kind]++;
      else if (r.get_ca())
        ++aa;
      else if (r.get_p())
        ++na;
    }
  aa += counts[ResidueInfo::AA] + counts[ResidueInfo::AAD];
  na += counts[ResidueInfo::RNA] + counts[ResidueInfo::DNA];
  if (aa == polymer.size() || (aa > 10 && 2 * aa > polymer.size()))
    return counts[ResidueInfo::AA] >= counts[ResidueInfo::AAD]
           ? PolymerType::PeptideL : PolymerType::PeptideD;
  if (na == polymer.size() || (na > 10 && 2 * na > polymer.size())) {
    if (counts[ResidueInfo::DNA] == 0)
      return PolymerType::Rna;
    else if (counts[ResidueInfo::RNA] == 0)
      return PolymerType::Dna;
    else
      return PolymerType::DnaRnaHybrid;
  }
  return PolymerType::Unknown;
}

inline bool is_polymer_residue(const Residue& res, PolymerType ptype) {
  ResidueInfo info = find_tabulated_residue(res.name);
  // If a standard residue is HETATM we assume that it is in the buffer.
  if (info.found() && info.is_standard() && res.het_flag == 'H')
    return false;
  switch (ptype) {
    case PolymerType::PeptideL:
    case PolymerType::PeptideD:
      // here we don't mind mixing D- and L- peptides
      return info.found() ? info.is_amino_acid() : !!res.get_ca();
    case PolymerType::Dna:
      return info.found() ? info.is_dna() : !!res.get_p();
    case PolymerType::Rna:
      return info.found() ? info.is_rna() : !!res.get_p();
    case PolymerType::DnaRnaHybrid:
      return info.found() ? info.is_nucleic_acid() : !!res.get_p();
    default:
      return false;
  }
}

inline bool are_connected(const Residue& r1, const Residue& r2,
                          PolymerType ptype) {
  if (is_polypeptide(ptype)) {
    // similar to has_peptide_bond_to()
    const Atom* a1 = r1.get_c();
    const Atom* a2 = r2.get_n();
    return a1 && a2 && a1->pos.dist_sq(a2->pos) < sq(1.341 * 1.5);
  }
  if (is_polynucleotide(ptype)) {
    const Atom* a1 = r1.get_o3prim();
    const Atom* a2 = r2.get_p();
    return a1 && a2 && a1->pos.dist_sq(a2->pos) < sq(1.6 * 1.5);
  }
  return false;
}

// not a good check, but requires only CA (or P) atoms
inline bool are_connected2(const Residue& r1, const Residue& r2,
                           PolymerType ptype) {
  if (is_polypeptide(ptype)) {
    const Atom* a1 = r1.get_ca();
    const Atom* a2 = r2.get_ca();
    return a1 && a2 && a1->pos.dist_sq(a2->pos) < sq(5.0);
  }
  if (is_polynucleotide(ptype)) {
    const Atom* a1 = r1.get_p();
    const Atom* a2 = r2.get_p();
    return a1 && a2 && a1->pos.dist_sq(a2->pos) < sq(7.5);
  }
  return false;
}

inline std::string make_one_letter_sequence(const SubChain& polymer) {
  std::string seq;
  const Residue* prev = nullptr;
  PolymerType ptype = check_polymer_type(polymer);
  for (const Residue& residue : polymer) {
    ResidueInfo info = find_tabulated_residue(residue.name);
    if (prev && !are_connected2(*prev, residue, ptype))
      seq += '-';
    seq += (info.one_letter_code != ' ' ? info.one_letter_code : 'X');
    prev = &residue;
  }
  return seq;
}

inline bool has_subchains_assigned(const Chain& chain) {
  return std::all_of(chain.residues.begin(), chain.residues.end(),
                     [](const Residue& r) { return !r.subchain.empty(); });
}

inline void add_entity_types(Chain& chain, bool overwrite) {
  PolymerType ptype = check_polymer_type(chain.whole());
  auto it = chain.residues.begin();
  for (; it != chain.residues.end(); ++it)
    if (overwrite || it->entity_type == EntityType::Unknown) {
      if (!is_polymer_residue(*it, ptype))
        break;
      it->entity_type = EntityType::Polymer;
    } else if (it->entity_type != EntityType::Polymer) {
      break;
    }
  for (; it != chain.residues.end(); ++it)
    if (overwrite || it->entity_type == EntityType::Unknown)
      it->entity_type = it->is_water() ? EntityType::Water
                                       : EntityType::NonPolymer;
}

inline void add_entity_types(Structure& st, bool overwrite) {
  for (Model& model : st.models)
    for (Chain& chain : model.chains)
      add_entity_types(chain, overwrite);
}

// The subchain field in the residue is where we store_atom_site.label_asym_id
// from mmCIF files. As of 2018 wwPDB software splits author's chains
// (auth_asym_id) into label_asym_id units:
// * linear polymer,
// * non-polymers (each residue has different separate label_asym_id),
// * and waters.
// Refmac/makecif is doing similar thing but using different naming and
// somewhat different rules (it was written in 1990's before PDBx/mmCIF).
//
// Here we use naming and rules different from both wwPDB and makecif.
inline void assign_subchains(Chain& chain) {
  int nonpoly_number = 0;
  for (Residue& res : chain.residues) {
    res.subchain = chain.name + ":";
    if (res.entity_type == EntityType::Polymer)
      res.subchain += '0';
    else if (res.entity_type == EntityType::NonPolymer)
      res.subchain += std::to_string(++nonpoly_number);
    else if (res.entity_type == EntityType::Water)
      res.subchain += 'w';
  }
}

inline void assign_subchains(Structure& st, bool force) {
  for (Model& model : st.models)
    for (Chain& chain : model.chains)
      if (force || !has_subchains_assigned(chain)) {
        add_entity_types(chain, false);
        assign_subchains(chain);
      }
}

inline void ensure_entities(Structure& st) {
  for (Model& model : st.models)
    for (Chain& chain : model.chains)
      for (SubChain sub : chain.subchains()) {
        Entity* ent = st.get_entity_of(sub);
        if (!ent) {
          EntityType etype = sub[0].entity_type;
          std::string name;
          if (etype == EntityType::Polymer)
            name = chain.name;
          else if (etype == EntityType::NonPolymer)
            name = sub[0].name + "!";
          else if (etype == EntityType::Water)
            name = "water";
          if (!name.empty()) {
            ent = &impl::find_or_add(st.entities, name);
            ent->entity_type = etype;
            ent->subchains.push_back(sub.name());
          }
        }
        // ensure we have polymer_type set where needed
        if (ent && ent->entity_type == EntityType::Polymer &&
            ent->polymer_type == PolymerType::Unknown)
          ent->polymer_type = check_polymer_type(sub);
      }
}


inline void deduplicate_entities(Structure& st) {
  for (auto i = st.entities.begin(); i != st.entities.end(); ++i)
    if (!i->poly_seq.empty())
      for (auto j = i + 1; j != st.entities.end(); ++j)
        if (j->polymer_type == i->polymer_type && j->poly_seq == i->poly_seq) {
          vector_move_extend(i->subchains, std::move(j->subchains));
          st.entities.erase(j--);
        }
}

inline void setup_entities(Structure& st) {
  assign_subchains(st, false);
  ensure_entities(st);
  deduplicate_entities(st);
}

// Remove hydrogens.
template<class T> void remove_hydrogens(T& obj) {
  for (auto& child : obj.children())
    remove_hydrogens(child);
}
template<> inline void remove_hydrogens(Residue& res) {
  vector_remove_if(res.atoms, [](const Atom& a) {
    return a.element == El::H || a.element == El::D;
  });
}

// Remove waters. It may leave empty chains.
template<class T> void remove_waters(T& obj) {
  for (auto& child : obj.children())
    remove_waters(child);
}
template<> inline void remove_waters(Chain& ch) {
  vector_remove_if(ch.residues,
                   [](const Residue& res) { return res.is_water(); });
}

// Remove ligands and waters. It may leave empty chains.
inline void remove_ligands_and_waters(Chain& ch) {
  PolymerType ptype = check_polymer_type(ch.whole());
  vector_remove_if(ch.residues, [&](const Residue& res) {
      if (res.entity_type == EntityType::Unknown) {
        // TODO: check connectivity
        return !is_polymer_residue(res, ptype);
      }
      return res.entity_type != EntityType::Polymer;
  });
}

inline void remove_ligands_and_waters(Structure& st) {
  for (Model& model : st.models)
    for (Chain& chain : model.chains)
      remove_ligands_and_waters(chain);
}

// Remove empty chains.
inline void remove_empty_chains(Model& m) {
  vector_remove_if(m.chains,
                   [](const Chain& chain) { return chain.residues.empty(); });
}
inline void remove_empty_chains(Structure& st) {
  for (Model& model : st.models)
    remove_empty_chains(model);
}

// Trim to alanine.
inline void trim_to_alanine(Chain& chain) {
  static const std::pair<std::string, El> ala_atoms[6] = {
    {"N", El::N}, {"CA", El::C}, {"C", El::C}, {"O", El::O}, {"CB", El::C},
    {"OXT", El::O}
  };
  for (Residue& res : chain.residues) {
    if (res.get_ca() == nullptr)
      return;  // we leave it; should we rather remove such residues?
    vector_remove_if(res.atoms, [](const Atom& a) {
        for (const auto& name_el : ala_atoms)
          if (a.name == name_el.first && a.element == name_el.second)
            return false;
        return true;
    });
  }
}

} // namespace gemmi
#endif
// vim:sw=2:ts=2:et
