//
// export LD_LIBRARY_PATH=/home/eic/lib64:${LD_LIBRARY_PATH}
//
// root -l
//
// root [0] gSystem->AddIncludePath("-I/home/eic/include");
// root [1] .x example_hepmc_writer.C+("out.hepmc", 100)
//

#include "HepMC3/GenEvent.h"
#include "HepMC3/ReaderAscii.h"
#include "HepMC3/WriterAscii.h"
#include "HepMC3/Print.h"

#include <iostream>
#include <random>
#include <cmath>
#include <math.h>

#include <TMath.h>
#include <TRandom.h>
#include <TDatabasePDG.h>

using namespace HepMC3;

void example_hepmc_writer(const char* out_fname, int n_events)
{
  auto *DatabasePDG = new TDatabasePDG();
  auto *pion = DatabasePDG->GetParticle(211), *kaon = DatabasePDG->GetParticle(321);

  WriterAscii hepmc_output(out_fname);
  int events_parsed = 0;
  GenEvent evt(Units::GEV, Units::MM);

  // Random number generator
  TRandom *rdmn_gen = new TRandom(0x12345678);

  for (events_parsed = 0; events_parsed < n_events; events_parsed++) {
    // type 4 is beam
    GenParticlePtr p1 =
        std::make_shared<GenParticle>(FourVector(0.0, 0.0, 12.0, 12.0), 11, 4);
    GenParticlePtr p2 = std::make_shared<GenParticle>(
        FourVector(0.0, 0.0, 100.0, 100.004), 2212, 4); 

    GenVertexPtr v1 = std::make_shared<GenVertex>();
    v1->add_particle_in(p1);
    v1->add_particle_in(p2);

    {
      Double_t eta   = rdmn_gen->Uniform(2.50, 2.51);
      Double_t th    = 2*std::atan(exp(-eta));
      Double_t p     = rdmn_gen->Uniform(7.0, 7.1);
      Double_t phi   = rdmn_gen->Uniform(0.0, 2*M_PI);

      Double_t px    = p * std::cos(phi) * std::sin(th);
      Double_t py    = p * std::sin(phi) * std::sin(th);
      Double_t pz    = p * std::cos(th);

      auto particle = events_parsed%2 ? pion : kaon;
      GenParticlePtr pq = std::make_shared<GenParticle>(FourVector(
								   px, py, pz,
								   sqrt(p*p + pow(particle->Mass(), 2))),
							particle->PdgCode(), 1);
      v1->add_particle_out(pq);
    }

    evt.add_vertex(v1);

    if (events_parsed == 0) {
      std::cout << "First event: " << std::endl;
      Print::listing(evt);
    }

    hepmc_output.write_event(evt);
    if (events_parsed % 10000 == 0) {
      std::cout << "Event: " << events_parsed << std::endl;
    }
    evt.clear();
  }
  hepmc_output.close();
  std::cout << "Events parsed and written: " << events_parsed << std::endl;
  exit(0);
}
