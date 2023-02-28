
#include "G4Box.hh"
#include "G4Tubs.hh"
#include "G4Trd.hh"
#include "G4Trap.hh"
#include "G4Cons.hh"
#include "G4LogicalVolume.hh"
#include "G4PVPlacement.hh"
#include <G4DataInterpolation.hh>
#include <G4LogicalBorderSurface.hh>
#include <G4UnionSolid.hh>
#include <G4SubtractionSolid.hh>
#include <G4IntersectionSolid.hh>
#include <G4GDMLParser.hh>

#define _GEANT_SOURCE_CODE_
#include <G4Object.h>

#include <tuning.h>

#include "DetectorConstruction.h"

#include <CherenkovDetectorCollection.h>
#include <CherenkovRadiator.h>
#include <G4RadiatorMaterial.h>
#include <CherenkovMirror.h>
#include <CherenkovPhotonDetector.h>

#if defined(BMF) && defined(_USE_MAGNETIC_FIELD_)
#include <G4TransportationManager.hh>
#include <G4FieldManager.hh>

#include <MagneticField.h>
#endif

// -------------------------------------------------------------------------------------

DetectorConstruction::DetectorConstruction(CherenkovDetectorCollection *geometry): 
G4VUserDetectorConstruction(), 
m_Geometry(geometry), m_gas_volume_length(0.0), m_gas_volume_radius(0.0), 
m_fiducial_volume_log(0), m_gas_volume_log(0)
{
} // DetectorConstruction::DetectorConstruction()

// -------------------------------------------------------------------------------------

void DetectorConstruction::ConstructSDandField()
{
#if defined(BMF) && defined(_USE_MAGNETIC_FIELD_)
  G4FieldManager* fieldMgr = G4TransportationManager::GetTransportationManager()->GetFieldManager();

  auto *mfield = new MagneticField(_USE_MAGNETIC_FIELD_);

  fieldMgr->SetDetectorField(mfield);
  fieldMgr->CreateChordFinder(mfield);
#endif
} // DetectorConstruction::ConstructSDandField()

// -------------------------------------------------------------------------------------

G4OpticalSurface *DetectorConstruction::CreateLambertianMirrorSurface(const char *name, 
								      double reflectivity, double roughness)
{
  G4OpticalSurface* opSurface = new G4OpticalSurface(name);
  opSurface->SetType(dielectric_metal);
	
  G4MaterialPropertiesTable *mpt = new G4MaterialPropertiesTable();

  if (roughness) {
    // 'ground': yes, I need specular lobe reflection in this case;
    opSurface->SetFinish(ground);
    opSurface->SetModel(unified);
    // NB: Fresnel refraction (or specular lobe reflection) will be sampled with respect 
    // to the surface with a bit "smeared" normal direction vector; 
    opSurface->SetSigmaAlpha(roughness);
    
    G4double specularLobe[_WLDIM_];
    
    for(int iq=0; iq<_WLDIM_; iq++) 
      // NB: yes, it is only a specular lobe reflection here;
      specularLobe[iq] = 1.00;
    
    mpt->AddProperty("SPECULARLOBECONSTANT", GetPhotonEnergies(), specularLobe, _WLDIM_);
  }
  else
    // 'polished': pure Fresnel reflection;
    opSurface->SetFinish(polished);

  G4double reflectivity_array[_WLDIM_];
  for(int iq=0; iq<_WLDIM_; iq++) 
    reflectivity_array[iq] = reflectivity;
  mpt->AddProperty("REFLECTIVITY", GetPhotonEnergies(), reflectivity_array, _WLDIM_);
  opSurface->SetMaterialPropertiesTable(mpt);

  return opSurface;
} // DetectorConstruction::CreateLambertianMirrorSurface()

// -------------------------------------------------------------------------------------

static G4UnionSolid *FlangeCut(double length, double clearance)
{
  // FIXME: do I really care about re-using the same names for these shapes?;
  auto *eflange = new G4Tubs("FlangeEpipe", 0.0, _FLANGE_EPIPE_DIAMETER_/2 + clearance, 
			     length/2, 0*degree, 360*degree);
  auto *hflange = new G4Tubs("FlangeHpipe", 0.0, _FLANGE_HPIPE_DIAMETER_/2 + clearance, 
			     length/2, 0*degree, 360*degree);
  // A wedge bridging two cylinders;
  double r0 = _FLANGE_EPIPE_DIAMETER_/2 + clearance, r1 = _FLANGE_HPIPE_DIAMETER_/2 + clearance;
  double L = _FLANGE_HPIPE_OFFSET_, a = r0*L/(r0-r1), b = r0*r0/a, c = r1*(a-b)/r0;
  // GEANT variables to define G4Trap;
  double pDz = length/2, pTheta = 0.0, pPhi = 0.0, pDy1 = (a - b - c)/2, pDy2 = pDy1; 
  double pDx1 = sqrt(r0*r0 - b*b), pDx2 = pDx1*r1/r0, pDx3 = pDx1, pDx4 = pDx2, pAlp1 = 0.0, pAlp2 = 0.0;
  auto *wedge = new G4Trap("FlangeWedge", pDz, pTheta, pPhi, pDy1, pDx1, pDx2, pAlp1, pDy2, pDx3, pDx4, pAlp2);
  G4RotationMatrix *rZ = new G4RotationMatrix(CLHEP::HepRotationZ(90*degree));
  auto *flange_shape = new G4UnionSolid("Tmp", eflange, hflange, 0, G4ThreeVector(_FLANGE_HPIPE_OFFSET_, 0.0, 0.0));

  return new G4UnionSolid("Tmp", flange_shape, wedge, rZ, G4ThreeVector(b + pDy1, 0.0, 0.0));
} // FlangeCut()

// -------------------------------------------------------------------------------------

//
// FIXME: a lot of duplicate code and a lot of hardcoded numbers;
//

void DetectorConstruction::BuildVesselWalls( void )
{
  // Inner vessel wall; it is part of the gas volume;
  {
    // FIXME: pyramid case, please;
    double wlength = m_gas_volume_length - _HRPPD_SUPPORT_GRID_BAR_HEIGHT_;
    auto outer = FlangeCut(wlength, _FLANGE_CLEARANCE_ + _VESSEL_INNER_WALL_THICKNESS_);
    auto *wall_shape = new G4SubtractionSolid("InnerWall", outer, 
					      FlangeCut(wlength + 1*mm, _FLANGE_CLEARANCE_),
					      0, G4ThreeVector(0.0, 0.0, 0.0));
    auto *wall_log = new G4LogicalVolume(wall_shape, m_QuarterInch_CF_HoneyComb,  "InnerWall", 0, 0, 0);
    new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, -_HRPPD_SUPPORT_GRID_BAR_HEIGHT_/2), wall_log, "InnerWall", 
		      m_gas_volume_log, false, 0);

    // Add a pair of aluminum reinforcement rings; full radial thickness (a bit more material);
    {
      double rlength = _INCH_/2;

      auto qouter = FlangeCut(rlength, _FLANGE_CLEARANCE_ + _VESSEL_INNER_WALL_THICKNESS_);
      auto *ring_shape = new G4SubtractionSolid("InnerWallAluRing", qouter, 
						FlangeCut(rlength + 1*mm, _FLANGE_CLEARANCE_),
						0, G4ThreeVector(0.0, 0.0, 0.0));
      auto *ring_log = new G4LogicalVolume(ring_shape, m_Aluminum,  "InnerWallAluRing", 0, 0, 0);
      for(unsigned iq=0; iq<2; iq++) {
	double zOffset = (iq ? 1.0 : -1.0)*(wlength/2 - rlength/2);

	new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, zOffset), ring_log, "InnerWallAluRing", wall_log, false, iq);
      } //for iq
    }
  }
  // Front vessel wall; it is part of the fiducial volume;
  {
    auto *wall_tube = new G4Tubs("FrontWall", 0.0, _VESSEL_OUTER_RADIUS_, _VESSEL_FRONT_SIDE_THICKNESS_/2, 
				 0*degree, 360*degree);
    auto *wall_shape = new G4SubtractionSolid("FrontWall", wall_tube, 
					      FlangeCut(_VESSEL_FRONT_SIDE_THICKNESS_ + 1*mm, _FLANGE_CLEARANCE_),
					      0, G4ThreeVector(0.0, 0.0, 0.0));
    auto *wall_log = new G4LogicalVolume(wall_shape, m_QuarterInch_CF_HoneyComb,  "FrontWall", 0, 0, 0);
    new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, -_FIDUCIAL_VOLUME_LENGTH_/2 + _VESSEL_FRONT_SIDE_THICKNESS_/2), 
		      wall_log, "FrontWall", m_fiducial_volume_log, false, 0);

    // Add inner reinforcement ring;
    {
      double rlength = _VESSEL_FRONT_SIDE_THICKNESS_;

      auto qouter = FlangeCut(rlength, _FLANGE_CLEARANCE_ + _INCH_/2);
      auto *ring_shape = new G4SubtractionSolid("FrontWallAluRing1", qouter, 
						FlangeCut(rlength + 1*mm, _FLANGE_CLEARANCE_),
						0, G4ThreeVector(0.0, 0.0, 0.0));
      auto *ring_log = new G4LogicalVolume(ring_shape, m_Aluminum,  "FrontWallAluRing1", 0, 0, 0);
      new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, 0.0), ring_log, "FrontWallAluRing1", wall_log, false, 0);
    }
    // Add outer reinforcement ring;
    {
      double rlength = _VESSEL_FRONT_SIDE_THICKNESS_;

      auto *ring_tube = new G4Tubs("FrontWallAluRing2", _VESSEL_OUTER_RADIUS_ - _INCH_/2, _VESSEL_OUTER_RADIUS_, 
				   rlength/2, 0*degree, 360*degree);

      auto *ring_log = new G4LogicalVolume(ring_tube, m_Aluminum,  "FrontWallAluRing2", 0, 0, 0);
      new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, 0.0), ring_log, "FrontWallAluRing2", wall_log, false, 0);
    }
  }
  // Outer vessel wall; it is part of the fiducial volume;
  {
    double wlength = m_gas_volume_length;
    auto *wall_tube = new G4Tubs("OuterWall", m_gas_volume_radius, _VESSEL_OUTER_RADIUS_, 
				 wlength/2, 0*degree, 360*degree);
    auto *wall_log = new G4LogicalVolume(wall_tube, m_HalfInch_CF_HoneyComb,  "OuterWall", 0, 0, 0);
    new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, -_FIDUCIAL_VOLUME_LENGTH_/2 + _VESSEL_FRONT_SIDE_THICKNESS_ + 
				       wlength/2), 
		      wall_log, "OuterWall", m_fiducial_volume_log, false, 0);

    // Add a pair of aluminum reinforcement rings; full radial thickness (a bit more material);
    {
      double rlength = _INCH_/2;

      auto *ring_tube = new G4Tubs("OuterWallAluRing", m_gas_volume_radius, _VESSEL_OUTER_RADIUS_, 
				   rlength/2, 0*degree, 360*degree);

      auto *ring_log = new G4LogicalVolume(ring_tube, m_Aluminum,  "OuterWallAluRing", 0, 0, 0);
      for(unsigned iq=0; iq<2; iq++) {
	double zOffset = (iq ? 1.0 : -1.0)*(wlength/2 - rlength/2);

	new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, zOffset), ring_log, "OuterWallAluRing", wall_log, false, iq);
      } //for iq
    }
  }
} // DetectorConstruction::BuildVesselWalls()

// -------------------------------------------------------------------------------------

G4VPhysicalVolume *DetectorConstruction::Construct( void )
{
  // Chemical elements and materials;
  DefineElements();
  DefineMaterials();

  // The experimental hall; FIXME: hardcoded;
  G4Tubs *expHall_box = new G4Tubs("World",  0*cm, 95*cm, 250*cm, 0*degree, 360*degree);
  auto expHall_log = new G4LogicalVolume(expHall_box, m_Air, "World", 0, 0, 0);
  G4VPhysicalVolume* expHall_phys = new G4PVPlacement(0, G4ThreeVector(), expHall_log, "World", 0, false, 0);
    
  auto *det = m_Geometry->GetDetector("pfRICH");
  //det->SetReadoutCellMask(~0x0);
  det->SetReadoutCellMask(0xFFFFFFFFFFFFFFFF);

  // Fiducial volume (air); has to be called "PFRICH";
  auto *fiducial_folume_tube = new G4Tubs("PFRICH", 0.0, _VESSEL_OUTER_RADIUS_, _FIDUCIAL_VOLUME_LENGTH_/2, 
				 0*degree, 360*degree);
  auto *fiducial_volume_shape = new G4SubtractionSolid("PFRICH", fiducial_folume_tube, 
						       FlangeCut(_FIDUCIAL_VOLUME_LENGTH_ + 1*mm, _FLANGE_CLEARANCE_), 
						       0, G4ThreeVector(0.0, 0.0, 0.0));
  m_fiducial_volume_log = new G4LogicalVolume(fiducial_volume_shape, m_Air,  "PFRICH", 0, 0, 0);
  // All volumes are defined assuming EIC h-going endcap orientation (dRICH case was developed this way 
  // for ATHENA); therefore need to rotate by 180 degrees around Y axis;
  G4RotationMatrix *rY = new G4RotationMatrix(CLHEP::HepRotationY(flip ? 180*degree : 0));
  auto *fiducial_volume_phys = new G4PVPlacement(rY, G4ThreeVector(0.0, 0.0, sign*_FIDUCIAL_VOLUME_OFFSET_), m_fiducial_volume_log, 
						 "PFRICH", expHall_phys->GetLogicalVolume(), false, 0);

  // Gas container volume;
  m_gas_volume_length = _FIDUCIAL_VOLUME_LENGTH_ - _VESSEL_FRONT_SIDE_THICKNESS_ - _SENSOR_AREA_LENGTH_;
  double _gas_volume_offset = -(_SENSOR_AREA_LENGTH_ - _VESSEL_FRONT_SIDE_THICKNESS_)/2;
  m_gas_volume_radius = _VESSEL_OUTER_RADIUS_ - _VESSEL_OUTER_WALL_THICKNESS_;
  auto *gas_tube = new G4Tubs("GasVolume", 0.0, m_gas_volume_radius, m_gas_volume_length/2, 0*degree, 360*degree);
  auto *gas_shape = new G4SubtractionSolid("GasVolume", gas_tube, 
					   // Yes, account for vessel inner wall thickness;
					   FlangeCut(m_gas_volume_length + 1*mm, _FLANGE_CLEARANCE_),
					   0, G4ThreeVector(0.0, 0.0, 0.0));
  m_gas_volume_log = new G4LogicalVolume(gas_shape, m_Nitrogen,  "GasVolume", 0, 0, 0);
  auto *gas_phys = new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, _gas_volume_offset), 
				     m_gas_volume_log, "GasVolume", m_fiducial_volume_log, false, 0);

  BuildVesselWalls();

  {
    // FIXME: Z-location does not really matter here, right?;
    auto boundary = new FlatSurface(TVector3(0,0,0), sign*TVector3(1,0,0), TVector3(0,-1,0));
#ifdef _DISABLE_GAS_VOLUME_PHOTONS_
    auto radiator = m_Geometry->SetContainerVolume(det, "GasVolume", 0, m_gas_volume_log, m_Nitrogen, boundary);
    radiator->DisableOpticalPhotonGeneration();
#else
    m_Geometry->SetContainerVolume(det, "GasVolume", 0, m_gas_volume_log, m_Nitrogen, boundary);
#endif
  }

  // To be used in boolean operations in several places;
  auto _flange = FlangeCut(m_gas_volume_length + 1*mm, _FLANGE_CLEARANCE_ + _VESSEL_INNER_WALL_THICKNESS_ + 
			   _BUILDING_BLOCK_CLEARANCE_);
  {
    // A running variable;
    double gzOffset = -m_gas_volume_length/2 + _BUILDING_BLOCK_CLEARANCE_;

    // min/max radii available for aerogel, mirrors and such; r0min can be an estimate, since a flange
    // cut will be applied at the end to all these shapes anyway;
    double r0min = _FLANGE_EPIPE_DIAMETER_/2 + _FLANGE_CLEARANCE_ + 
      _VESSEL_INNER_WALL_THICKNESS_ + _BUILDING_BLOCK_CLEARANCE_;
    double r0max = m_gas_volume_radius - _BUILDING_BLOCK_CLEARANCE_;

    // Aerogel;
    {
      // FIXME: yes, for now hardcoded;
      const unsigned adim[_AEROGEL_BAND_COUNT_] = {8, 14, 20};
      //double rheight = (r0max - r0min - (_AEROGEL_BAND_COUNT_+1)*_AEROGEL_FRAME_WALL_THICKNESS_) / _AEROGEL_BAND_COUNT_;
      double rheight = (r0max - r0min - (_AEROGEL_BAND_COUNT_-1)*_AEROGEL_SEPARATOR_WALL_THICKNESS_ - 
			_AEROGEL_INNER_WALL_THICKNESS_ - _AEROGEL_OUTER_WALL_THICKNESS_) / _AEROGEL_BAND_COUNT_;

      // Up to two aerogel layers;
      for(unsigned il=0; il<2; il++) {
	double agthick = 0.0;
	G4RadiatorMaterial *aerogel = 0;
#ifdef _AEROGEL_1_ 
	if (!il) {
	  agthick = _AEROGEL_THICKNESS_1_;
	  aerogel   = _m_Aerogel[_AEROGEL_1_];
	} //if
#else
	if (!il) continue;
#endif
#ifdef _AEROGEL_2_ 
	if ( il) {
	  agthick = _AEROGEL_THICKNESS_2_;
	  aerogel = _m_Aerogel[_AEROGEL_2_];
	} //if
#else
	if ( il) continue;
#endif
	
	{
	  gzOffset += agthick/2;
	  
	  // First aerogel sectors and azimuthal spacers;
	  CherenkovRadiator *radiator = 0;
	  for(unsigned ir=0; ir<_AEROGEL_BAND_COUNT_; ir++) {
	    int counter = ir ? -1 : 0;
	    double apitch = 360*degree / adim[ir];
	    //double r0 = r0min + (ir+1)*_AEROGEL_FRAME_WALL_THICKNESS_ + ir*rheight, r1 = r0 + rheight, rm = (r0+r1)/2;
	    double r0 = r0min + _AEROGEL_INNER_WALL_THICKNESS_ + ir*(_AEROGEL_SEPARATOR_WALL_THICKNESS_ + rheight);
	    double r1 = r0 + rheight, rm = (r0+r1)/2;

	    // Calculate angular space occupied by the spacers and by the tiles; no gas gaps for now;
	    // assume that a wegde shape is good enough (GEANT visualization does not like boolean objects), 
	    // rather than creating constant thicjkess azimuthal spacers; just assume that spacer thickness is 
	    // _AEROGEL_FRAME_WALL_THICKNESS_ at r=rm;
	    double l0 = 2*M_PI*rm/adim[ir], l1 = _AEROGEL_SEPARATOR_WALL_THICKNESS_, lsum = l0 + l1;

	    // FIXME: names overlap in several places!;
	    double wd0 = (l0/lsum)*(360*degree / adim[ir]), wd1 = (l1/lsum)*(360*degree / adim[ir]);
	    TString ag_name = "Tmp", sp_name = "Tmp"; 
	    if (ir) ag_name.Form("%s-%d-00", aerogel->GetName().c_str(), ir);
	    if (ir) sp_name.Form("A-Spacer--%d-00",                      ir);
	    G4Tubs *ag_tube  = new G4Tubs(ag_name.Data(), r0, r1, agthick/2, 0*degree, wd0);
	    G4Tubs *sp_tube  = new G4Tubs(sp_name.Data(), r0, r1, agthick/2,      wd0, wd1);

	    for(unsigned ia=0; ia<adim[ir]; ia++) {
	      G4RotationMatrix *rZ    = new G4RotationMatrix(CLHEP::HepRotationZ(    ia*apitch));
	      G4RotationMatrix *rZinv = new G4RotationMatrix(CLHEP::HepRotationZ(-1.*ia*apitch));

	      G4LogicalVolume *ag_log = 0, *sp_log = 0;
	      if (ir) {
		ag_log = new G4LogicalVolume(ag_tube,                   aerogel, ag_name.Data(), 0, 0, 0);
		sp_log = new G4LogicalVolume(sp_tube, _AEROGEL_SPACER_MATERIAL_, sp_name.Data(), 0, 0, 0);
		counter++;
	      } else {
		ag_name.Form("%s-%d-%02d", aerogel->GetName().c_str(), ir, ia);
		auto ag_shape = new G4SubtractionSolid(ag_name.Data(), ag_tube, _flange, 
						       rZinv, G4ThreeVector(0.0, 0.0, 0.0));
		ag_log = new G4LogicalVolume(ag_shape, aerogel, ag_name.Data(),   0, 0, 0);

		sp_name.Form("A-Spacer--%d-%02d",                      ir, ia);
		auto sp_shape = new G4SubtractionSolid(sp_name.Data(), sp_tube, _flange, 
						       rZinv, G4ThreeVector(0.0, 0.0, 0.0));
		sp_log = new G4LogicalVolume(sp_shape, _AEROGEL_SPACER_MATERIAL_, sp_name.Data(),   0, 0, 0);
	      } //if
	      if (!ir && !ia) {
		TVector3 nx(1*sign,0,0), ny(0,-1,0);
		
		auto surface = new FlatSurface(sign*(1/mm)*TVector3(0,0,_FIDUCIAL_VOLUME_OFFSET_ + _gas_volume_offset + gzOffset), nx, ny);
		radiator = m_Geometry->AddFlatRadiator(det, aerogel->GetName(), CherenkovDetector::Upstream, 
						       0, ag_log, aerogel, surface, agthick/mm);
	      }
	      else
		// This of course assumes that optical surfaces are the same (no relative tilts between bands, etc);
		m_Geometry->AddRadiatorLogicalVolume(radiator, ag_log);

	      new G4PVPlacement(rZ, G4ThreeVector(0.0, 0.0, gzOffset), ag_log, ag_name.Data(), m_gas_volume_log, false, counter);
	      new G4PVPlacement(rZ, G4ThreeVector(0.0, 0.0, gzOffset), sp_log, sp_name.Data(), m_gas_volume_log, false, counter);
	    } //for ia
	  } //for ir

	  // Then the radial spacers; 
	  {
	    double accu = r0min;

	    for(unsigned ir=0; ir<_AEROGEL_BAND_COUNT_+1; ir++) {
	      double thickness = ir ? (ir == _AEROGEL_BAND_COUNT_ ? _AEROGEL_OUTER_WALL_THICKNESS_ : 
				       _AEROGEL_SEPARATOR_WALL_THICKNESS_) : _AEROGEL_INNER_WALL_THICKNESS_;
	      //double r0 = r0min + ir*(rheight + _AEROGEL_FRAME_WALL_THICKNESS_), r1 = r0 + _AEROGEL_FRAME_WALL_THICKNESS_;
	      double r0 = accu, r1 = r0 + thickness;
	      
	      TString sp_name = "Tmp"; if (ir) sp_name.Form("R-Spacer--%d-00", ir);
	      
	      G4Tubs *sp_tube  = new G4Tubs(sp_name.Data(), r0, r1, agthick/2, 0*degree, 360*degree);
	      
	      G4LogicalVolume *sp_log = 0;
	      if (ir) 
		sp_log = new G4LogicalVolume(sp_tube, _AEROGEL_SPACER_MATERIAL_, sp_name.Data(), 0, 0, 0);
	      else {
		sp_name.Form("R-Spacer--%d-00", ir);
		auto sp_shape = new G4SubtractionSolid(sp_name.Data(), sp_tube, _flange, 0, G4ThreeVector(0.0, 0.0, 0.0));
		sp_log = new G4LogicalVolume(sp_shape, _AEROGEL_SPACER_MATERIAL_, sp_name.Data(),   0, 0, 0);
	      } //if
	      
	      new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, gzOffset), sp_log, sp_name.Data(), m_gas_volume_log, false, 0);

	      accu += thickness + rheight;
	    } //for ir
	  }	    

	  // FIXME: not really needed that big between the two layers?;
	  gzOffset += agthick/2 + _BUILDING_BLOCK_CLEARANCE_;
	}
      } //for il
    }

    // Acrylic;
#ifdef _ACRYLIC_THICKNESS_
    {
      double acthick = _ACRYLIC_THICKNESS_;
      gzOffset += acthick/2;

      G4Tubs *ac_tube  = new G4Tubs("Tmp", r0min, r0max, acthick/2, 0*degree, 360*degree);
      auto ac_shape = new G4SubtractionSolid("Acrylic", ac_tube, _flange, 0, G4ThreeVector(0.0, 0.0, 0.0));
      G4LogicalVolume* ac_log = new G4LogicalVolume(ac_shape, m_Acrylic,  "Acrylic", 0, 0, 0);
      {
	TVector3 nx(1*sign,0,0), ny(0,-1,0);
	
	auto surface = new FlatSurface(sign*(1/mm)*TVector3(0,0,_FIDUCIAL_VOLUME_OFFSET_ + _gas_volume_offset + gzOffset), nx, ny);
	auto radiator = m_Geometry->AddFlatRadiator(det, "Acrylic", CherenkovDetector::Upstream, 
						    0, ac_log, m_Acrylic, surface, acthick/mm);
#ifdef _DISABLE_ACRYLIC_PHOTONS_
	radiator->DisableOpticalPhotonGeneration();
#endif
      }
      
      new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, gzOffset), ac_log, "Acrylic", m_gas_volume_log, false, 0);

      gzOffset += acthick/2 + _BUILDING_BLOCK_CLEARANCE_;
    }
#endif

    // Book optical boundaries of the inner and outer conical mirrors; do not want to spoil the 
    // central area (mirro-less) optical path (would happen if define them in situ);
    OpticalBoundary *mboundaries[2] = {0, 0};

    {
      const char *names[2] = {"InnerMirror", "OuterMirror"};
      double mlen = m_gas_volume_length/2 - gzOffset /*- _SENSOR_AREA_LENGTH_*/ - _BUILDING_BLOCK_CLEARANCE_;
#ifdef _USE_PYRAMIDS_
      mlen -= _BUILDING_BLOCK_CLEARANCE_ + _PYRAMID_MIRROR_HEIGHT_;
#else
      mlen -= _BUILDING_BLOCK_CLEARANCE_ + _HRPPD_SUPPORT_GRID_BAR_HEIGHT_;
#endif
      double mpos = gzOffset + mlen/2;
      double r0[2] = {r0min, r0max}, r1[2] = {_CONICAL_MIRROR_INNER_RADIUS_, _CONICAL_MIRROR_OUTER_RADIUS_};

      for(unsigned im=0; im<2; im++) {
	auto material = im ? m_HalfInch_CF_HoneyComb : m_QuarterInch_CF_HoneyComb;
	double thickness = im ? _OUTER_MIRROR_THICKNESS_ : _INNER_MIRROR_THICKNESS_;
	//auto mgroup = new CherenkovMirrorGroup();
	
	{
	  //auto mshape = im ? new G4Cons(names[im], r0[im] - thickness, r0[im], r1[im] - thickness, r1[im], mlen/2, 0*degree, 360*degree) :
	  auto mshape = im ? new G4Cons(names[im], r0[im], r0[im] + thickness, r1[im], r1[im] + thickness, mlen/2, 0*degree, 360*degree) :
	    new G4Cons(names[im], r0[im] - thickness, r0[im], r1[im] - thickness, r1[im], mlen/2, 0*degree, 360*degree);
	  
	  // There should be a cutaway on the inner mirror because of the beam pipe flange;
	  G4LogicalVolume *solid_log = 0;
	  if (im) {
	    auto solid = new G4IntersectionSolid(names[im], mshape, gas_tube, 0, G4ThreeVector(0.0, 0.0, 0.0));
	    solid_log = new G4LogicalVolume(solid, material, names[im], 0, 0, 0);
	  } else {
	    auto solid = new G4SubtractionSolid(names[im], mshape, _flange,  0, G4ThreeVector(0.0, 0.0, 0.0));
	    solid_log = new G4LogicalVolume(solid, material, names[im], 0, 0, 0);
	  } //if
	  
	  // FIXME: duplicate code;
	  G4VisAttributes* visAtt = new G4VisAttributes(G4Colour(0, 0, 1, 0.5));
	  visAtt->SetVisibility(true);
	  visAtt->SetForceSolid(true);
	  
	  solid_log->SetVisAttributes(visAtt);
	  //} //if

	  // NB: geometry will be saved in [mm] throughout the code;
	  auto mirror = new ConicalMirror(mshape, material, sign*(1/mm)*TVector3(0.0, 0.0, _FIDUCIAL_VOLUME_OFFSET_ + _gas_volume_offset + mpos),
					  sign*TVector3(0,0,1), r0[im]/mm, r1[im]/mm, mlen/mm);
	  
	  mirror->SetColor(G4Colour(0, 0, 1, 0.5));
	  mirror->SetReflectivity(_MIRROR_REFLECTIVITY_, this);
	  
	  // Mimic mirror->PlaceWedgeCopies() call; FIXME: can be vastly simplified for this simple case;
	  mirror->DefineLogicalVolume();
	  G4VPhysicalVolume *phys = new G4PVPlacement(/*rZ*/0, G4ThreeVector(0,0,mpos), solid_log,// ? solid_log : mirror->GetLogicalVolume(), 
						      mirror->GetSolid()->GetName(), 
						      gas_phys->GetLogicalVolume(), false, 0);//m_Copies.size());
	  mirror->AddCopy(mirror->CreateCopy(phys));
	  {
	    auto msurface = mirror->GetMirrorSurface();
	    
	    if (msurface)
	      // Do I really need them separately?;
	      //char buffer[128]; snprintf(buffer, 128-1, "SphericalMirror");//Surface");//%2d%02d", io, iq);
	      new G4LogicalBorderSurface(mirror->GetSolid()->GetName(), gas_phys, phys, msurface);
	  } 

	  auto mcopy = dynamic_cast<SurfaceCopy*>(mirror->GetCopy(0));//m_Copies[iq]);
	  mcopy->m_Surface = dynamic_cast<ParametricSurface*>(mirror)->_Clone(0.0, TVector3(0,0,1));
	  if (!im) dynamic_cast<ConicalSurface*>(mcopy->m_Surface)->SetConvex();
	  
	  {
	    //mgroup->AddMirror(mirror);
	    m_Geometry->AddMirrorLookupEntry(mirror->GetLogicalVolume(), mirror);
	    
	    auto surface = dynamic_cast<SurfaceCopy*>(mirror->GetCopy(0))->m_Surface;
	    mboundaries[im] = new OpticalBoundary(m_Geometry->FindRadiator(m_gas_volume_log), surface, false);
	    
	    // Complete the radiator volume description; this is the rear side of the container gas volume;
	    //+? det->GetRadiator("GasVolume")->m_Borders[0].second = surface;
	  }
	}
      } //for im
    }

    // Photon detectors; 
    {
      //zOffset = gas_volume_length/2 - _SENSOR_AREA_LENGTH_;
      double azOffset = _FIDUCIAL_VOLUME_LENGTH_/2 - _SENSOR_AREA_LENGTH_;
      double xysize = _HRPPD_TILE_SIZE_, wndthick = _HRPPD_WINDOW_THICKNESS_, zwnd = azOffset + wndthick/2;

      // HRPPD assembly container volume;
      double hrppd_container_volume_thickness = 30*mm, zcont = azOffset + hrppd_container_volume_thickness/2;
      G4Box *hrppd_box  = new G4Box("HRPPD", xysize/2, xysize/2, hrppd_container_volume_thickness/2);
      G4LogicalVolume* hrppd_log = new G4LogicalVolume(hrppd_box, m_Nitrogen,  "HRPPD", 0, 0, 0);

      // Full size quartz window;
      G4Box *wnd_box  = new G4Box("QuartzWindow", xysize/2, xysize/2, wndthick/2);
      G4LogicalVolume* wnd_log = new G4LogicalVolume(wnd_box, m_FusedSilica,  "QuartzWindow", 0, 0, 0);
      {	
	TVector3 nx(1*sign,0,0), ny(0,-1,0);
	
	// A single entry; this assumes of course that all the windows are at the same Z, and parallel to each other;
	auto surface = new FlatSurface(sign*(1/mm)*TVector3(0,0,_FIDUCIAL_VOLUME_OFFSET_ /*+ gas_volume_offset*/ + zwnd), nx, ny);
#ifdef _DISABLE_HRPPD_WINDOW_PHOTONS_
	auto radiator = m_Geometry->AddFlatRadiator(det, "QuartzWindow", CherenkovDetector::Downstream, 
						    0, wnd_log, m_FusedSilica, surface, wndthick/mm);
	radiator->DisableOpticalPhotonGeneration();
#else
	m_Geometry->AddFlatRadiator(det, "QuartzWindow", CherenkovDetector::Downstream, 
				    0, wnd_log, m_FusedSilica, surface, wndthick/mm);
#endif
      }	

      double pitch = xysize + _HRPPD_INSTALLATION_GAP_, xyactive = _HRPPD_ACTIVE_AREA_SIZE_;
      double xyopen = _HRPPD_OPEN_AREA_SIZE_;
      double certhick = _HRPPD_CERAMIC_BODY_THICKNESS_, zcer = azOffset + wndthick + certhick/2;

      // HRPPD body imitation; ignore MCPs (small fraction compared to the ceramic body);
      G4Box *cer_box  = new G4Box("CeramicBox", xysize/2, xysize/2, certhick/2);
      G4Box *cut_box  = new G4Box("CeramicCut", xyopen/2, xyopen/2, certhick/2);
      auto ceramic = new G4SubtractionSolid("CeramicBody", cer_box, cut_box, 0, 
					    G4ThreeVector(0,0, -_HRPPD_BASEPLATE_THICKNESS_));
      G4LogicalVolume* cer_log = new G4LogicalVolume(ceramic, m_Ceramic,  "CeramicBody", 0, 0, 0);
      // FIXME: duplicate code;
      {
	G4VisAttributes* visAtt = new G4VisAttributes(G4Colour(1, 1, 1, 0.5));
	visAtt->SetVisibility(true);
	visAtt->SetForceSolid(true);
	
	cer_log->SetVisAttributes(visAtt);
      }
      G4Box *plating  = new G4Box("Plating", xyopen/2, xyopen/2, _HRPPD_PLATING_LAYER_THICKNESS_/2);
      G4LogicalVolume* plating_log = new G4LogicalVolume(plating, m_Silver,  "Plating", 0, 0, 0);

      //{
	double accu = -hrppd_container_volume_thickness/2;

	// Window layer;
	auto wnd_phys = new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, accu + wndthick/2), wnd_log, 
					  "QuartzWindow", hrppd_log, false, 0);
	accu += wndthick;

	// Ceramic pictureframe body behind it;
	auto cer_phys = new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, accu + certhick/2), cer_log,
					  "CeramicBody", hrppd_log, false, 0);
	// Place plating layer in the middle;
	new G4PVPlacement(                0, G4ThreeVector(0.0, 0.0, accu + certhick/2), plating_log, 
					  "Plating",     hrppd_log, false, 0);
	//accu += certhick;

	// Rough reflective optical border between them;
	G4OpticalSurface* opWindowMetallization = 
	  CreateLambertianMirrorSurface("WindowMetallization", _HRPPD_METALLIZATION_REFLECTIVITY_, _HRPPD_METALLIZATION_ROUGHNESS_);
	new G4LogicalBorderSurface("WindowMetallization", wnd_phys, cer_phys, opWindowMetallization);
	//}
      double pdthick = 0.01*mm, zpdc = azOffset + wndthick + pdthick/2;
      G4Box *pd_box  = new G4Box("PhotoDetector", xyactive/2, xyactive/2, pdthick/2);
      auto pd = new CherenkovPhotonDetector(pd_box, m_Bialkali);
      pd->SetCopyIdentifierLevel(1);
      pd->DefineLogicalVolume();
      pd->SetColor(G4Colour(1, 0, 0, 1.0));
      // Cannot access GEANT shapes in the reconstruction code -> store this value;
      pd->SetActiveAreaSize(xyactive);
      new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, accu + pdthick/2), pd->GetLogicalVolume(), "PhotoDetector", 
			hrppd_log, false, 0);
      //accu += pdthick;

#if 0
      const unsigned ddim = 3;
      G4LogicalVolume *dlogs[ddim];
      const char *dnames[ddim] = {"PCB", "Copper", "Water"};
      G4Material *dmats[ddim]  = {m_FR4, m_Copper, m_Water};
      double area = xysize*xysize, dthicks[ddim] = {_READOUT_PCB_THICKNESS_, 
						    _EFFECTIVE_WATER_VOLUME_/area, 
						    _EFFECTIVE_COPPER_VOLUME_/area}; 
      // FIXME: duplicate code;
      {
	G4VisAttributes* visAtt = new G4VisAttributes(G4Colour(0, 1, 0, 0.5));
	visAtt->SetVisibility(true);
	visAtt->SetForceSolid(true);
      
	for(unsigned iq=0; iq<ddim; iq++) {
	  auto box = new G4Box(dnames[iq], xysize/2, xysize/2, dthicks[iq]/2);
	  dlogs[iq] = new G4LogicalVolume(box, dmats[iq], dnames[iq], 0, 0, 0);
	  
	  dlogs[iq]->SetVisAttributes(visAtt);
	} //for iq      
      }
#endif

      // 'pitch': yes, want them installed without gaps;
#ifdef _USE_PYRAMIDS_
      G4Box *pyra_box = new G4Box("Dummy", pitch/2, pitch/2, _PYRAMID_MIRROR_HEIGHT_/2);
      G4Trd *pyra_cut = new G4Trd("Dummy", pitch/2, xyactive/2, pitch/2, xyactive/2, _PYRAMID_MIRROR_HEIGHT_/2 + 0.01*mm);
      auto *pyra_shape = new G4SubtractionSolid("MirrorPyramid", pyra_box, pyra_cut);
      
      // NB: geometry will be saved in [mm] throughout the code;
      auto pyramid = new CherenkovMirror(pyra_shape, m_Absorber);
      
      pyramid->SetColor(G4Colour(0, 1, 1, 0.5));
      pyramid->SetReflectivity(_MIRROR_REFLECTIVITY_, this);
	
      // Mimic the essential part of the mirror->PlaceWedgeCopies() call;
      pyramid->DefineLogicalVolume();
      m_Geometry->AddMirrorLookupEntry(pyramid->GetLogicalVolume(), pyramid);
#else
#if 0
      double alu_thickness = _INCH_/2 - _HRPPD_SUPPORT_GRID_BAR_HEIGHT_;//3.0*mm;
      auto *alu_tube = new G4Tubs("AluFrame", 0.0, _VESSEL_OUTER_RADIUS_, alu_thickness/2, 0*degree, 360*degree);
      auto *alu_shape = new G4SubtractionSolid("AluFrame", alu_tube, 
					       FlangeCut(alu_thickness + 1*mm, _FLANGE_CLEARANCE_), 0, G4ThreeVector(0.0, 0.0, 0.0));
      double alu_cut_size = xysize + 0.5*mm;
      auto *alu_cut = new G4Box("AluWndCut", alu_cut_size/2, alu_cut_size/2, alu_thickness/2 + 1*mm);
      // Cut the central area by hand;
      alu_shape = new G4SubtractionSolid("AluFrame", alu_shape, alu_cut, 0, G4ThreeVector(                       0.0, 0.0, 0.0));
      alu_shape = new G4SubtractionSolid("AluFrame", alu_shape, alu_cut, 0, G4ThreeVector(_HRPPD_CENTRAL_ROW_OFFSET_, 0.0, 0.0));
#endif
#if 1
      G4Box *grid_box = new G4Box("Dummy", pitch/2, pitch/2, _HRPPD_SUPPORT_GRID_BAR_HEIGHT_/2);
      double value = pitch - _HRPPD_SUPPORT_GRID_BAR_WIDTH_;
      G4Box *grid_cut = new G4Box("Dummy", value/2, value/2, _HRPPD_SUPPORT_GRID_BAR_HEIGHT_/2 + 0.01*mm);
      auto *grid_shape = new G4SubtractionSolid("SupportGridBar", grid_box, grid_cut);
      auto *grid_log = new G4LogicalVolume(grid_shape, m_CarbonFiber, "SupportGridBar", 0, 0, 0);
      // FIXME: duplicate code;
      {
	G4VisAttributes* visAtt = new G4VisAttributes(G4Colour(0, 1, 1, 0.5));
	visAtt->SetVisibility(true);
	visAtt->SetForceSolid(true);
	
	grid_log->SetVisAttributes(visAtt);
      }
#endif
#endif

      {
#ifdef _USE_SiPM_QE_
	const G4int qeEntries = 15;
      
	// Create S13660-3050AE-08 SiPM QE table; FIXME: or was it S13661?;
	double WL[qeEntries] = { 325,  340,  350,  370,  400,  450,  500,  550,  600,  650,  700,  750,  800,  850,  900};
	double QE[qeEntries] = {0.04, 0.10, 0.20, 0.30, 0.35, 0.40, 0.38, 0.35, 0.27, 0.20, 0.15, 0.12, 0.08, 0.06, 0.04};
#else
	const G4int qeEntries = 26;
	
	// Create LAPPD QE table; use LAPPD126 from Alexey's March 2022 LAPPD Workshop presentation;
	double WL[qeEntries] = { 160,  180,  200,  220,  240,  260,  280,  300,  320,  340,  360,  380,  400,  
				 420,  440,  460,  480,  500,  520,  540,  560,  580,  600,  620,  640,  660};
	double QE[qeEntries] = {0.25, 0.26, 0.27, 0.30, 0.32, 0.35, 0.36, 0.36, 0.36, 0.36, 0.37, 0.35, 0.30, 
				0.27, 0.24, 0.20, 0.18, 0.15, 0.13, 0.11, 0.10, 0.09, 0.08, 0.07, 0.05, 0.05};
#endif     
	
	double qemax = 0.0;
	G4double qePhotonEnergy[qeEntries], qeData[qeEntries];
	for(int iq=0; iq<qeEntries; iq++) {
	  qePhotonEnergy[iq] = eV * _MAGIC_CFF_ / (WL[qeEntries - iq - 1] + 0.0);
	  qeData        [iq] =                     QE[qeEntries - iq - 1];
	  
	  if (qeData[iq] > qemax) qemax = qeData[iq];
	} //for iq
	
	pd->SetQE(eV * _MAGIC_CFF_ / WL[qeEntries-1], eV * _MAGIC_CFF_ / WL[0], 
		  // NB: last argument: want a built-in selection of unused photons, which follow the QE(lambda);
		  // see CherenkovSteppingAction::UserSteppingAction() for a usage case;
		  new G4DataInterpolation(qePhotonEnergy, qeData, qeEntries, 0.0, 0.0), qemax ? 1.0/qemax : 1.0);
	pd->SetGeometricEfficiency(_SENSOR_PLANE_GEOMETRIC_EFFICIENCY_ * _SAFETY_FACTOR_);
      }

      m_Geometry->AddPhotonDetector(det, pd->GetLogicalVolume(), pd);

      {
	const unsigned ydim = 5, qpop[ydim] = {9, 9, 9, 7, 5};

	std::vector<TVector2> coord;

	assert(qpop[0]%2);
	{
	  double yOffset = 0.0;
	  
	  unsigned xdim = qpop[0];
	  for(unsigned ix=0; ix<xdim; ix++) {
	    double xOffset = pitch*(ix - (xdim-1)/2.) + (ix <= 3 ? 0.0 : _HRPPD_CENTRAL_ROW_OFFSET_);
	    	    
	    if (ix == 4) continue;

	    coord.push_back(TVector2(xOffset, yOffset));
	  }
	} 
	for(unsigned iy=1; iy<ydim; iy++) {
	  for(unsigned bt=0; bt<2; bt++) {
	    double qsign = bt ? -1.0 : 1.0, yOffset = qsign*pitch*iy;
	    
	    unsigned xdim = qpop[iy];
	    for(unsigned ix=0; ix<xdim; ix++) {
	      double xOffset = pitch*(ix - (xdim-1)/2.);
	      
	      coord.push_back(TVector2(xOffset, yOffset));
	    }
	  } //for bt
	} //for iy

	{
	  int counter = 0;

	  for(auto xy: coord) {
	    {
#if 1
	      new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), zcont), hrppd_log, "HRPPD", m_fiducial_volume_log, false, counter);
#else
	      // Window layer;
	      auto wnd_phys = new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), zwnd), wnd_log,     "QuartzWindow", m_fiducial_volume_log, false, counter);
	      // Ceramic pictureframe body behind it;
	      auto cer_phys = new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), zcer), cer_log,     "CeramicBody",  m_fiducial_volume_log, false, counter);
	      // Place plating layer in the middle;
	      new G4PVPlacement(                0, G4ThreeVector(xy.X(), xy.Y(), zcer), plating_log, "Plating",      m_fiducial_volume_log, false, counter);
#endif

	      // Rough reflective optical border between them;
	      //+G4OpticalSurface* opWindowMetallization = 
	      //+CreateLambertianMirrorSurface("WindowMetallization", _HRPPD_METALLIZATION_REFLECTIVITY_, _HRPPD_METALLIZATION_ROUGHNESS_);
	      //+new G4LogicalBorderSurface("WindowMetallization", wnd_phys, cer_phys, opWindowMetallization);

	      // Dead material layers;
#if 0
	      {
		double accu = zcer + 10*mm;

		for(unsigned iq=0; iq<ddim; iq++) {
		  new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), accu + dthicks[iq]/2), dlogs[iq], dnames[iq], m_fiducial_volume_log, false, counter);
		  accu += dthicks[iq];
		} //for iq
	      }
#endif
	    }

	    // Cut in the aluminum plate;
	    {
	      //static unsigned counter;
	      //if (counter++ < 100 && fabs(sqrt(xy.X()*xy.X()+xy.Y()*xy.Y())) < 150.0)
	      //+alu_shape = new G4SubtractionSolid("AluFrame", alu_shape, alu_cut, 0, G4ThreeVector(xy.X(), xy.Y(), 0.0)); 
	    }

	    //new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), zpdc), pd->GetLogicalVolume(), "PhotoDetector", m_fiducial_volume_log, false, counter);
	    auto surface = new FlatSurface((1/mm)*TVector3(sign*xy.X(), xy.Y(),sign*(_FIDUCIAL_VOLUME_OFFSET_ /*+ gas_volume_offset*/ + zpdc)), 
					   TVector3(1*sign,0,0), TVector3(0,-1,0));
	    //auto surface = new FlatSurface((1/mm)*TVector3(sign*xy.X(), xy.Y(),sign*_FIDUCIAL_VOLUME_OFFSET_ /*+ gas_volume_offset*/ + zpdc), 
	    //				   TVector3(1*sign,0,0), TVector3(0,-1,0));
	    
#ifdef _USE_PYRAMIDS_
	    {
	      // FIXME: need to take care about overlap with the inner wall;
	      assert(0);
	      auto pyra_phys = 
		new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), zOffset - _PYRAMID_MIRROR_HEIGHT_/2), 
				  pyramid->GetLogicalVolume(), pyramid->GetSolid()->GetName(), m_fiducial_volume_log, false, counter);

	      new G4LogicalBorderSurface(pyramid->GetSolid()->GetName(), gas_phys, pyra_phys, pyramid->GetMirrorSurface());
	    }
#else
	    // Yes, they are part of the gas volume;
	    //new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), azOffset - _HRPPD_SUPPORT_GRID_BAR_HEIGHT_/2), 
	    new G4PVPlacement(0, G4ThreeVector(xy.X(), xy.Y(), m_gas_volume_length/2 - _HRPPD_SUPPORT_GRID_BAR_HEIGHT_/2), 
	    		      grid_log, "SupportGridBar", m_gas_volume_log, false, counter);
	    
#endif

	    {
#if defined(_USE_PYRAMIDS_) && defined(_USE_PYRAMIDS_OPTICS_)
	      // Calculate and store the pyramid mirror flat surfaces; 
	      OpticalBoundary *pboundaries[4];
	      for(unsigned iq=0; iq<4; iq++)
		switch (iq) {
		case 0:;
		case 1:;
		case 2:;
		case 3:;
		  // Reflection off one of the four pyramid mirrors;
		  {		  
		    double vx = (pitch - xyactive)/2, vy = _PYRAMID_MIRROR_HEIGHT_, norm = sqrt(vx*vx+vy*vy);
		    double z0 = _FIDUCIAL_VOLUME_OFFSET_ + gas_volume_offset + zOffset - _PYRAMID_MIRROR_HEIGHT_/2;
		    
		    TVector3 nx( vx/norm, 0, -vy/norm), ny(0,-1,0), nz(0,0,1), nv(0,1,0);
		    nx.Rotate(iq*M_PI/2, nz); ny.Rotate(iq*M_PI/2, nz);
		    
		    TVector3 center((pitch + xyactive)/4, 0.0, z0); 
		    center.Rotate(iq*M_PI/2, nz); center += TVector3(xy.X(), xy.Y(), 0.0);
		    if (flip) {
		      center.Rotate(M_PI, nv);
		      nx.Rotate(M_PI, nv);
		      ny.Rotate(M_PI, nv);
		    } //if
		    auto qsurface = new FlatSurface((1/mm)*center, nx, ny);
		    
		    auto boundary = new OpticalBoundary(m_Geometry->FindRadiator(m_gas_volume_log), qsurface, false);
		    pboundaries[iq] = boundary;
		    det->StoreOpticalBoundary(boundary);
		  }
		  break;
		} //switch .. for iq 
#endif
	      
	      // Four "global" optical configurations considered, see the first switch below;
	      for(unsigned iq=0; iq<4; iq++) {
#ifndef _USE_CONICAL_MIRROR_OPTICS_
		if (iq) continue;
#endif

		// And five more cases at the sensor end, see the second switch below;
		for(unsigned ip=0; ip<5; ip++) {
#if !defined(_USE_PYRAMIDS_) || !defined(_USE_PYRAMIDS_OPTICS_)
		  if (ip != 4) continue;
#endif
		  // Mimic det->CreatePhotonDetectorInstance();
		  unsigned sector = 0, icopy = counter;
		  auto irt = pd->AllocateIRT(sector, icopy);
		  
		  // Aerogel and acrylic;
		  if (det->m_OpticalBoundaries[CherenkovDetector::Upstream].find(sector) != 
		      det->m_OpticalBoundaries[CherenkovDetector::Upstream].end())
		    for(auto boundary: det->m_OpticalBoundaries[CherenkovDetector::Upstream][sector])
		      irt->AddOpticalBoundary(boundary);
		  
		  // FIXME: will the gas-quartz boundary be described correctly in this sequence?;
		  switch (iq) {
		  case 0: 
		    // Direct hit;
		    break;
		  case 1:
		  case 2:         
		    // Reflection on either inner or outer mirrors;
		    irt->AddOpticalBoundary(mboundaries[iq-1]);
		    break;
		  case 3:
		    // Reflection on outer, then on inner mirror; happens at large angles; if the pyramids are
		    // too high, these photons will undergo more reflections, and cannot be saved;
		    irt->AddOpticalBoundary(mboundaries[1]);
		    irt->AddOpticalBoundary(mboundaries[0]);
		    break;
		  } //switch
		  
#if defined(_USE_PYRAMIDS_) && defined(_USE_PYRAMIDS_OPTICS_)
		  switch (ip) {
		  case 0:;
		  case 1:;
		  case 2:;
		  case 3:;
		    irt->AddOpticalBoundary(pboundaries[ip]);
		    break;
		  case 4: 
		    // Direct hit;
		    break;
		  } //switch
#endif
		  
		  // Quartz windows;
		  if (det->m_OpticalBoundaries[CherenkovDetector::Downstream].find(sector) != 
		      det->m_OpticalBoundaries[CherenkovDetector::Downstream].end())
		    for(auto boundary: det->m_OpticalBoundaries[CherenkovDetector::Downstream][sector])
		      irt->AddOpticalBoundary(boundary);
		  
		  // Terminate the optical path;
		  pd->AddItselfToOpticalBoundaries(irt, surface);
		}
	      } //for ip
	    } //for iq

	    counter++;
	  } //for xy
	} 
      }

#if 0
      auto *alu_log = new G4LogicalVolume(alu_shape, m_Aluminum,  "AluFrame", 0, 0, 0);
      // FIXME: duplicate code;
      {
	G4VisAttributes* visAtt = new G4VisAttributes(G4Colour(0, 1, 1, 0.5));
	// Otherwise visualization hangs on this shape;
	visAtt->SetVisibility(false);//true);
	visAtt->SetForceSolid(true);
	
	alu_log->SetVisAttributes(visAtt);
      }
      new G4PVPlacement(0, G4ThreeVector(0.0, 0.0, _FIDUCIAL_VOLUME_LENGTH_/2 - _SENSOR_AREA_LENGTH_ + alu_thickness/2),// + 20*mm), 
	alu_log, "AluFrame", m_fiducial_volume_log, false, 0);
#endif
    }
    
    for(unsigned im=0; im<2; im++)
      // FIXME: they are not really upstream (just need to store them);
      det->AddOpticalBoundary(CherenkovDetector::Upstream, 0, mboundaries[im]);
    //#endif
  }

  for(auto radiator: det->Radiators())
    radiator.second->SetReferenceRefractiveIndex(radiator.second->GetMaterial()->RefractiveIndex(eV*_MAGIC_CFF_/_LAMBDA_NOMINAL_));

#ifdef _GENERATE_GDML_OUTPUT_
  {
    G4GDMLParser parser;
    unlink("pfRICH.gdml");
    parser.Write("pfRICH.gdml", fiducial_volume_phys);
  }
#endif

  return expHall_phys;
} // DetectorConstruction::Construct()

// -------------------------------------------------------------------------------------
