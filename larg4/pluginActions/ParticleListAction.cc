////////////////////////////////////////////////////////////////////////
/// \file  ParticleListAction.cxx
/// \brief Use Geant4's user "hooks" to maintain a list of particles generated by Geant4.
///
/// \author  seligman@nevis.columbia.edu
///
/// Design considerations
/// ---------------------
///
/// This class relies on the MCTruth index from
/// g4b::PrimaryParticleInformation to operate correctly.  This index
/// is an integer value that corresponds to an MCTruth object, as
/// accessed through art::Handle<std::vector<simb::MCTruth>> objects.
/// However, the order in which MCTruth objects are processed must be
/// consistent between this service and the MCTruthEventAction
/// service, which creates the PrimaryParticleInformation object,
/// otherwise the Assns objects created here will be incorrect.
///
/// Through art 3.09, one can rely on the order returned by a given
/// Event::getMany call to be predictable and consistent within the
/// same program.  However, this behavior should not necessarily be
/// relied upon, and a different implementation of this class would
/// insulate users from such details, making the implementation
/// simpler.  One should determine whether storing an art::ProductID
/// object along with an MCTruthIndex might be more helpful.
///
////////////////////////////////////////////////////////////////////////

#include "larg4/pluginActions/ParticleListAction_service.h"

#include "lardataobj/Simulation/sim.h"

#include "nug4/G4Base/PrimaryParticleInformation.h"

// Framework includes
#include "fhiclcpp/ParameterSet.h"

// ROOT includes
#include "TLorentzVector.h"

#include "CLHEP/Units/SystemOfUnits.h"

// Geant4 includes
#include "Geant4/G4DynamicParticle.hh"
#include "Geant4/G4ParticleDefinition.hh"
#include "Geant4/G4PrimaryParticle.hh"
#include "Geant4/G4StepPoint.hh"
#include "Geant4/G4ThreeVector.hh"
#include "Geant4/G4Track.hh"
#include "Geant4/G4VProcess.hh"
#include "Geant4/G4VUserPrimaryParticleInformation.hh"

#include "range/v3/view.hpp"

// STL includes
#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <vector>

namespace larg4 {

  //----------------------------------------------------------------------------
  // Constructor.
  ParticleListActionService::ParticleListActionService(fhicl::ParameterSet const& p)
    : artg4tk::EventActionBase("PLASEventActionBase")
    , artg4tk::TrackingActionBase("PLASTrackingActionBase")
    , artg4tk::SteppingActionBase("PLASSteppingActionBase")
    , fenergyCut(p.get<double>("EnergyCut", 0.0 * CLHEP::GeV))
    , fstoreTrajectories(p.get<bool>("storeTrajectories", true))
    , fkeepGenTrajectories(p.get<std::vector<std::string>>("keepGenTrajectories", {}))
    , fKeepEMShowerDaughters(p.get<bool>("keepEMShowerDaughters", true))
    , fNotStoredPhysics(p.get<std::vector<std::string>>("NotStoredPhysics", {}))
    , fkeepOnlyPrimaryFullTraj(p.get<bool>("keepOnlyPrimaryFullTrajectories", false))
    , fSparsifyTrajectories(p.get<bool>("SparsifyTrajectories", false))
    , fSparsifyMargin(p.get<double>("SparsifyMargin", 0.015))
    , fKeepTransportation(p.get<bool>("KeepTransportation", false))
    , fKeepSecondToLast(p.get<bool>("KeepSecondToLast", false))
    , fStoreDroppedMCParticles(p.get<bool>("StoreDroppedMCParticles", false))
    , fdroppedParticleList(fStoreDroppedMCParticles ? std::make_unique<sim::ParticleList>() : nullptr)

  {
    // -- D.R. If a custom list of not storable physics is provided, use it, otherwise
    //    use the default list. This preserves the behavior of the keepEmShowerDaughters
    //    parameter
    bool customNotStored = not fNotStoredPhysics.empty();
    if (!fKeepEMShowerDaughters) {
      // -- Don't keep all processes
      if (!customNotStored) // -- Don't keep but haven't provided a list
      {                     // -- default list of not stored physics
        fNotStoredPhysics = {"conv",
                             "LowEnConversion",
                             "Pair",
                             "compt",
                             "Compt",
                             "Brem",
                             "phot",
                             "Photo",
                             "Ion",
                             "annihil"};
      }

      std::stringstream sstored;
      sstored << "The full tracking information will not be stored for particles"
              << " resulting from the following processes: \n{ ";
      for (auto const& i : fNotStoredPhysics) {
        sstored << "\"" << i << "\" ";
        fNotStoredCounterUMap.emplace(i, 0); // -- initialize counter
      }
      mf::LogInfo("ParticleListActionService") << sstored.str() << "}\n";
    }
    else { // -- Keep all processes
      mf::LogInfo("ParticleListActionService")
        << "Storing full tracking information for all processes. \n";
      if (customNotStored) // -- custom list will be ignored
      {
        mf::LogWarning("StoredPhysics")
          << "NotStoredPhysics provided, but will be ignored."
          << " To use NotStoredPhysics, set keepEMShowerDaughters to false";
      }
    }

    // -- sparsify info
    if (fSparsifyTrajectories)
      mf::LogInfo("ParticleListActionService")
        << "Trajectory sparsification enabled with SparsifyMargin : " << fSparsifyMargin << "\n";
    // Specify dropped particle list
    // fdroppedParticleList(fStoreDroppedMCParticles ? std::make_unique<sim::ParticleList>() :
    //                                                  nullptr);
  } // end constructor

  //----------------------------------------------------------------------------
  // Begin the event
  void ParticleListActionService::beginOfEventAction(const G4Event*)
  {
    // Clear any previous particle information.
    fCurrentParticle.clear();
    fParticleList.clear();
    fParentIDMap.clear();
    fTargetIDMap.clear();
    fMCTIndexMap.clear();
    fMCTPrimProcessKeepMap.clear();
    fCurrentTrackID = sim::NoParticleId;
    fTrackIDOffset = 0;
    fPrimaryTruthMap.clear();
    fMCTIndexToGeneratorMap.clear();
    fNotStoredCounterUMap.clear();
    fdroppedTracksMap.clear();
    if (fdroppedParticleList) fdroppedParticleList->clear();
    // -- D.R. If a custom list of keepGenTrajectories is provided, use it, otherwise
    //    keep or drop decision made based storeTrajectories parameter. This preserves
    //    the behavior of the storeTrajectories fhicl param
    bool customKeepTraj = not fkeepGenTrajectories.empty();
    if (!fstoreTrajectories) { // -- fstoreTrajectories : false
      mf::LogDebug("beginOfEventAction::Generator") << "Trajectory points will not be stored.";
    }
    else if (!customKeepTraj) { // -- fstoretrajectories : true and empty keepGenTrajectories list
      mf::LogDebug("beginOfEventAction::Generator")
        << "keepGenTrajectories list is empty. Will"
        << " store trajectory points for all generators";
    }

    // -- D.R. determine mapping between MCTruthIndex(s) and generator(s) for later reference
    size_t nKeep = 0;
    std::string generator_name = "unknown";
    for (size_t mcti = 0; mcti < fMCLists->size(); ++mcti) {

      std::stringstream sskeepgen;
      sskeepgen << "MCTruth object summary :";
      sskeepgen << "\n\tPrimary MCTIndex : " << mcti;

      // -- Obtain the generator (provenance) corresponding to the mctruth index:
      auto const& mclistHandle = (*fMCLists)[mcti];
      generator_name = mclistHandle.provenance()->inputTag().label();
      sskeepgen << "\n\tProvenance/Generator : " << generator_name;

      G4bool keepGen = false;
      if (fstoreTrajectories) // -- storeTrajectories set to true; check which
      {
        if (!customKeepTraj) { // -- no custom list, so keep all
          keepGen = true;
          nKeep++;
        }
        else { // -- custom list, so check the ones in the event against provided keep list
          for (auto keepableGen : fkeepGenTrajectories) {
            if (generator_name == keepableGen) { // -- exit upon finding match; false by default
              keepGen = true;
              nKeep++;
              break;
            }
          }
        }
      }
      fMCTIndexToGeneratorMap.emplace(mcti, std::make_pair(generator_name, keepGen));
      sskeepgen << "\n\tTrajectory points storable : " << (keepGen ? "true" : "false") << "\n";
      mf::LogDebug("beginOfEventAction::Generator") << sskeepgen.str();
    }

    if (nKeep == 0 && customKeepTraj && fstoreTrajectories) {
      mf::LogWarning("beginOfEventAction::keepableGenerators")
        << "storeTrajectories"
        << " set to true and a non-empty keepGenTrajectories list provided in configuration file, "
           "but"
        << " none of the generators in this list are present in the event! Double check list or "
           "don't"
        << " provide keepGenTrajectories in the configuration to keep all trajectories from all"
        << " generator labels. This may be expected for generators that have a nonzero probability "
           "of"
        << " producing no particles (e.g. some radiologicals)";
    }
  }

  //-------------------------------------------------------------
  // figure out the ultimate parentage of the particle with track ID
  // trackid
  // assume that the current track id has already been added to
  // the fParentIDMap
  int ParticleListActionService::GetParentage(int trackid) const
  {
    int parentid = sim::NoParticleId;

    // search the fParentIDMap recursively until we have the parent id
    // of the first EM particle that led to this one
    auto itr = fParentIDMap.find(trackid);
    while (itr != fParentIDMap.end()) {
      // set the parentid to the current parent ID, when the loop ends
      // this id will be the first EM particle
      parentid = (*itr).second;
      itr = fParentIDMap.find(parentid);
    }

    return parentid;
  }

  //----------------------------------------------------------------------------
  // Create our initial simb::MCParticle object and add it to the sim::ParticleList.
  void ParticleListActionService::preUserTrackingAction(const G4Track* track)
  {
    // Particle type.
    G4ParticleDefinition* particleDefinition = track->GetDefinition();
    G4int pdgCode = particleDefinition->GetPDGEncoding();

    // Get Geant4's ID number for this track.  This will be the same
    // ID number that we'll use in the ParticleList.
    // It is offset by the number of tracks accumulated from the previous Geant4
    // runs (if any)
    int const trackID = track->GetTrackID() + fTrackIDOffset;
    fCurrentTrackID = trackID;
    fTargetIDMap[trackID] = fCurrentTrackID;
    // And the particle's parent (same offset as above):
    int parentID = track->GetParentID() + fTrackIDOffset;

    std::string process_name = "unknown";
    std::string mct_primary_process = "unknown";
    bool isFromMCTProcessPrimary = false;
    bool notstore = false;

    // Is there an MCTruth object associated with this G4Track?  We
    // have to go up a "chain" of information to find out:
    const G4DynamicParticle* dynamicParticle = track->GetDynamicParticle();
    const G4PrimaryParticle* primaryParticle = dynamicParticle->GetPrimaryParticle();
    simb::GeneratedParticleIndex_t primaryIndex = simb::NoGeneratedParticleIndex;
    size_t primarymctIndex = 0;
    if (primaryParticle != nullptr) {
      const G4VUserPrimaryParticleInformation* gppi = primaryParticle->GetUserInformation();
      const g4b::PrimaryParticleInformation* ppi =
        dynamic_cast<const g4b::PrimaryParticleInformation*>(gppi);
      if (ppi != nullptr) {
        primaryIndex = ppi->MCParticleIndex();
        primarymctIndex = ppi->MCTruthIndex();
        mct_primary_process = ppi->GetMCParticle()->Process();

        // If we've made it this far, a PrimaryParticleInformation
        // object exists and we are using a primary particle, set the
        // process name accordingly

        // -- D.R. : if process == "primary" exactly (this will most likely be the case), mark it as
        //    a primary with keepable trajectory for itself and its descendants.
        // -- elsif it simply starts with "primary", accept it but mark it as non-keep
        // -- else force it to be primary because we have determined that it is a primary particle
        //    This was the original behavior of the code i.e. process was _set_ to "primary"
        //    regardless of what the process name was in the generator MCTruth object.
        //
        // -- NOTE: This enforces a convention for process names assigned in the gen stage.
        if (mct_primary_process.compare("primary") == 0) {
          process_name = "primary";
          isFromMCTProcessPrimary = true;
        }
        else if (mct_primary_process.find("primary") == 0) {
          process_name = mct_primary_process;
          isFromMCTProcessPrimary = false;
          mf::LogDebug("PrimaryParticle") << "MCTruth primary process name contains \"primary\" "
                                          << " but is not solely \"primary\" : " << process_name
                                          << ".\nWill not store full set of trajectory points.";
        }
        else { // -- override it
          process_name = "primary";
          isFromMCTProcessPrimary = true;
          mf::LogWarning("PrimaryParticle")
            << "MCTruth primary process does not beging with string"
            << " literal \"primary\" : " << process_name << "\nOVERRIDING it to \"primary\"";
        }
        // -- The process_name check is simply to allow an additional way to reduce memory usage,
        //    namely, by creating MCTruth input particles with multiple process labels (e.g. "primary"
        //    and "primaryBackground") that can be used to veto storage of trajectory points.

        // primary particles should have parentID = 0, even if there
        // are multiple MCTruths for this event
        parentID = 0;
      } // end else no primary particle information
    }   // Is there a G4PrimaryParticle?
    // If this is not a primary particle...
    else {
      // check if this particle was made in an undesirable process. For example:
      // if one is not interested in EM shower particles, don't put it in the particle
      // list as one wouldn't care about secondaries, tertiaries, etc. For these showers
      // figure out what process is making this track - skip it if it is
      // one of pair production, compton scattering, photoelectric effect
      // bremstrahlung, annihilation, or ionization
      process_name = track->GetCreatorProcess()->GetProcessName();
      if (!fKeepEMShowerDaughters) {
        for (auto const& p : fNotStoredPhysics) {
          if (process_name.find(p) != std::string::npos) {
            notstore = true;
            mf::LogDebug("NotStoredPhysics") << "Found process : " << process_name;

            int old = 0;
            auto search = fNotStoredCounterUMap.find(p);
            if (search != fNotStoredCounterUMap.end()) { old = search->second; }
            fNotStoredCounterUMap.insert_or_assign(p, (old + 1));
            break;
          }
        }
        if (notstore) {
          // figure out the ultimate parentage of this particle
          // first add this track id and its parent to the fParentIDMap
          fParentIDMap[trackID] = parentID;
          fCurrentTrackID = -1 * this->GetParentage(trackID);
          // check that fCurrentTrackID is in the particle list - it is possible
          // that this particle's parent is a particle that did not get tracked.
          // An example is a parent that was made due to muMinusCaptureAtRest
          // and the daughter was made by the phot process. The parent likely
          // isn't saved in the particle list because it is below the energy cut
          // which will put a bogus track id value into the sim::IDE object for
          // the sim::SimChannel if we don't check it.
          if (!fParticleList.KnownParticle(fCurrentTrackID)) fCurrentTrackID = sim::NoParticleId;
          fTargetIDMap[trackID] = fCurrentTrackID;
          // clear current particle as we are not stepping this particle and
          // adding trajectory points to it
          fdroppedTracksMap[this->GetParentage(trackID)].insert(trackID);
          //fCurrentParticle.clear();
          //return;
        } // end if process matches an undesired process
      }   // end if not keeping EM shower daughters

      // Check the energy of the particle.  If it falls below the energy
      // cut, don't add it to our list.
      G4double energy = track->GetKineticEnergy();
      if (energy < fenergyCut && pdgCode != 0) {
        fdroppedTracksMap[this->GetParentage(trackID)].insert(trackID);
        fCurrentParticle.clear();
        // do add the particle to the parent id map though
        // and set the current track id to be it's ultimate parent
        fParentIDMap[trackID] = parentID;
        fCurrentTrackID = -1 * this->GetParentage(trackID);
        fTargetIDMap[trackID] = fCurrentTrackID;
        return;
      }

      // check to see if the parent particle has been stored in the particle navigator
      // if not, then see if it is possible to walk up the fParentIDMap to find the
      // ultimate parent of this particle.  Use that ID as the parent ID for this
      // particle
      if (!fParticleList.KnownParticle(parentID) &&
          !(fdroppedParticleList && fdroppedParticleList->KnownParticle(parentID))) {
        // do add the particle to the parent id map
        // just in case it makes a daughter that we have to track as well
        fParentIDMap[trackID] = parentID;
        int pid = this->GetParentage(parentID);

        // if we still can't find the parent in the particle navigator,
        // we have to give up
        if (!fParticleList.KnownParticle(pid) &&
          !(fdroppedParticleList && fdroppedParticleList->KnownParticle(parentID))) {
          MF_LOG_WARNING("ParticleListActionService")
            << "can't find parent id: " << parentID << " in the particle list, or fParentIDMap."
            << " Make " << parentID << " the mother ID for"
            << " track ID " << fCurrentTrackID << " in the hope that it will aid debugging.";
        }
        else
          parentID = pid;
      }

      // Once the parentID is secured, inherit the MCTruth Index
      // which should have been set already
      if (auto it = fMCTIndexMap.find(parentID); it != cend(fMCTIndexMap)) {
        primarymctIndex = it->second;
      }
      else {
        throw art::Exception(art::errors::LogicError)
          << "Could not locate MCT Index for parent trackID of " << parentID;
      }

      // Inherit whether the parent is from a primary with MCTruth process_name == "primary"
      isFromMCTProcessPrimary = fMCTPrimProcessKeepMap[parentID];
    } // end if not a primary particle

    // This is probably the PDG mass, but just in case:
    double mass = dynamicParticle->GetMass() / CLHEP::GeV;

    // Create the sim::Particle object.
    fCurrentParticle.clear();
    fCurrentParticle.particle =
      new simb::MCParticle{trackID, pdgCode, process_name, parentID, mass};
    fCurrentParticle.truthIndex = primaryIndex;

    fMCTIndexMap[trackID] = primarymctIndex;

    fMCTPrimProcessKeepMap[trackID] = isFromMCTProcessPrimary;

    // -- determine whether full set of trajectorie points should be stored or only the start and end points
    fCurrentParticle.keepFullTrajectory =
      (!fstoreTrajectories) ?
        false : /*don't want trajectory points at all, bail*/
        (!(fMCTIndexToGeneratorMap[primarymctIndex].second)) ?
        false : /*particle is not from a storable generator*/
          (!fkeepOnlyPrimaryFullTraj) ?
        true : /*want all primaries tracked for a storable generator*/
            (isFromMCTProcessPrimary) ?
        true :       /*only descendants from primaries with MCTruth process == "primary"*/
              false; /*not from MCTruth process "primary"*/
    // Polarization.
    const G4ThreeVector& polarization = track->GetPolarization();
    fCurrentParticle.particle->SetPolarization(
      TVector3{polarization.x(), polarization.y(), polarization.z()});

    if (track->GetProperTime() != 0) { return; }

    // if KeepEMShowerDaughters = False and we decided to drop this particle,
    // record it before throwing it away.
    if (notstore) { // this bool checks if particle is eliminated by NotStoredPhysics
      if (fdroppedParticleList) fdroppedParticleList->Add(fCurrentParticle.particle);
      return;
    }
    // Save the particle in the ParticleList.
    fParticleList.Add(fCurrentParticle.particle);
  }

  //----------------------------------------------------------------------------
  void ParticleListActionService::postUserTrackingAction(const G4Track* aTrack)
  {
    if (!fCurrentParticle.hasParticle()) return;

    if (aTrack) {
      fCurrentParticle.particle->SetWeight(aTrack->GetWeight());

      // Get the post-step information from the G4Step.
      const G4StepPoint* postStepPoint = aTrack->GetStep()->GetPostStepPoint();
      if (!postStepPoint->GetProcessDefinedStep()) {
        // Now we get to do some awkward cleanup because the
        // fParticleList was augmented during the
        // preUserTrackingAction.  We cannot call 'Archive' because
        // that only sets the mapped type of the entry to
        // nullptr...which is really bad whenever we iterate through
        // entries in the endOfEventAction and dereference the mapped
        // type.  We have to entirely erase the entry.
        auto key_to_erase = fParticleList.key(fCurrentParticle.particle);
        fParticleList.erase(key_to_erase);
        if (!fCurrentParticle.keepFullTrajectory && fdroppedParticleList){ 
          //fdroppedParticleList->erase(key_to_erase); // also erase from dropped list
          fdroppedParticleList->Archive(fCurrentParticle.particle); //Archive in case LArG4 is configured to keep minimal version
        }
        // after the particle is archived, it is deleted
        fCurrentParticle.clear();
        return;
      }

      G4String process = postStepPoint->GetProcessDefinedStep()->GetProcessName();
      fCurrentParticle.particle->SetEndProcess(process);

      // -- D.R. Store the final point only for particles that have not had intermediate trajectory
      //    points saved. This avoids double counting the final trajectory point for particles from
      //    generators with storable trajectory points.

      if (!fCurrentParticle.keepFullTrajectory) {
        const G4ThreeVector position = postStepPoint->GetPosition();
        G4double time = postStepPoint->GetGlobalTime();

        // Remember that LArSoft uses cm, ns, GeV.
        TLorentzVector fourPos(position.x() / CLHEP::cm,
                               position.y() / CLHEP::cm,
                               position.z() / CLHEP::cm,
                               time / CLHEP::ns);

        const G4ThreeVector momentum = postStepPoint->GetMomentum();
        const G4double energy = postStepPoint->GetTotalEnergy();
        TLorentzVector fourMom(momentum.x() / CLHEP::GeV,
                               momentum.y() / CLHEP::GeV,
                               momentum.z() / CLHEP::GeV,
                               energy / CLHEP::GeV);

        // Add another point in the trajectory.
        AddPointToCurrentParticle(fourPos, fourMom, std::string(process));
      }
      // -- particle has a full trajectory, apply SparsifyTrajectory method if enabled
      else if (fSparsifyTrajectories) {
        fCurrentParticle.particle->SparsifyTrajectory(fSparsifyMargin, fKeepSecondToLast);
      }
    }

    // store truth record pointer, only if it is available
    if (fCurrentParticle.isPrimary()) {
      fPrimaryTruthMap[fCurrentParticle.particle->TrackId()] = fCurrentParticle.truthInfoIndex();
    }

    return;
  }

  //----------------------------------------------------------------------------
  // With every step, add to the particle's trajectory.
  void ParticleListActionService::userSteppingAction(const G4Step* step)
  {
    // N.B. G4 guarantees that following are non-null:
    //  - step
    //  - step->GetPostStepPoint()
    if (!fCurrentParticle.hasParticle() || !step->GetPostStepPoint()->GetProcessDefinedStep()) {
      return;
    }
    // Temporary fix for problem where  DeltaTime on the first step
    // of optical photon propagation is calculated incorrectly. -wforeman
    double const globalTime = step->GetTrack()->GetGlobalTime();
    double const velocity_G4 = step->GetTrack()->GetVelocity();
    double const velocity_step = step->GetStepLength() / step->GetDeltaTime();
    if ((step->GetTrack()->GetDefinition()->GetPDGEncoding() == 0) &&
        fabs(velocity_G4 - velocity_step) > 0.0001) {
      // Subtract the faulty step time from the global time,
      // and add the correct step time based on G4 velocity.
      step->GetPostStepPoint()->SetGlobalTime(globalTime - step->GetDeltaTime() +
                                              step->GetStepLength() / velocity_G4);
    }

    // For the most part, we just want to add the post-step
    // information to the particle's trajectory.  There's one
    // exception: In PreTrackingAction, the correct time information
    // is not available.  So add the correct vertex information here.

    if (fCurrentParticle.particle->NumberTrajectoryPoints() == 0) {

      // Get the pre/along-step information from the G4Step.
      const G4StepPoint* preStepPoint = step->GetPreStepPoint();

      const G4ThreeVector position = preStepPoint->GetPosition();
      G4double time = preStepPoint->GetGlobalTime();

      // Remember that LArSoft uses cm, ns, GeV.
      TLorentzVector fourPos(position.x() / CLHEP::cm,
                             position.y() / CLHEP::cm,
                             position.z() / CLHEP::cm,
                             time / CLHEP::ns);

      const G4ThreeVector momentum = preStepPoint->GetMomentum();
      const G4double energy = preStepPoint->GetTotalEnergy();
      TLorentzVector fourMom(momentum.x() / CLHEP::GeV,
                             momentum.y() / CLHEP::GeV,
                             momentum.z() / CLHEP::GeV,
                             energy / CLHEP::GeV);

      // Add the first point in the trajectory.
      AddPointToCurrentParticle(fourPos, fourMom, "Start");
    } // end if this is the first step

    // At this point, the particle is being transported through the
    // simulation.
    // change below:
    // This method is being called for every step that
    // the track passes through, but we don't want to update the
    // trajectory information if the step  was defined by the StepLimiter.
    G4String process = step->GetPostStepPoint()->GetProcessDefinedStep()->GetProcessName();
    G4bool ignoreProcess = process.contains("StepLimiter");

    // We store the initial creation point of the particle
    // and its final position (ie where it has no more energy, or at least < 1 eV) no matter
    // what, but whether we store the rest of the trajectory depends
    // on the process, and on a user switch.
    // -- D.R. Store additional trajectory points only for desired generators and processes
    if (!ignoreProcess && fCurrentParticle.keepFullTrajectory) {

      // Get the post-step information from the G4Step.
      const G4StepPoint* postStepPoint = step->GetPostStepPoint();

      const G4ThreeVector position = postStepPoint->GetPosition();
      G4double time = postStepPoint->GetGlobalTime();

      // Remember that LArSoft uses cm, ns, GeV.
      TLorentzVector fourPos(position.x() / CLHEP::cm,
                             position.y() / CLHEP::cm,
                             position.z() / CLHEP::cm,
                             time / CLHEP::ns);

      const G4ThreeVector momentum = postStepPoint->GetMomentum();
      const G4double energy = postStepPoint->GetTotalEnergy();
      TLorentzVector fourMom(momentum.x() / CLHEP::GeV,
                             momentum.y() / CLHEP::GeV,
                             momentum.z() / CLHEP::GeV,
                             energy / CLHEP::GeV);

      // Add another point in the trajectory.
      AddPointToCurrentParticle(fourPos, fourMom, std::string(process));
    }
  }

  //----------------------------------------------------------------------------
  /// Utility class for the EndOfEventAction method: update the
  /// daughter relationships in the particle list.
  class UpdateDaughterInformation {
  public:
    explicit UpdateDaughterInformation(sim::ParticleList& p) : particleList{&p} {}
    void operator()(sim::ParticleList::value_type& particleListEntry) const
    {
      // We're looking at this Particle in the list.
      int particleID = particleListEntry.first;

      // The parent ID of this particle;
      // we ask the particle list since the particle itself might have been lost
      // ("archived"), but the particle list still holds the information we need
      int parentID = particleList->GetMotherOf(particleID);

      // If the parentID <= 0, this is a primary particle.
      if (parentID <= 0) return;

      // If we get here, this particle is somebody's daughter.  Add
      // it to the list of daughter particles for that parent.

      // Get the parent particle from the list.
      sim::ParticleList::iterator parentEntry = particleList->find(parentID);

      if (parentEntry == particleList->end()) {
        // We have an "orphan": a particle whose parent isn't
        // recorded in the particle list.  This is not signficant;
        // it's possible for a particle not to be saved in the list
        // because it failed an energy cut, but for it to have a
        // daughter that passed the cut (e.g., a nuclear decay).
        return;
      }
      if (!parentEntry->second) return; // particle archived, nothing to update

      // Add the current particle to the daughter list of the parent.
      simb::MCParticle* parent = parentEntry->second;
      parent->AddDaughter(particleID);
    }

  private:
    sim::ParticleList* particleList;
  };

  //----------------------------------------------------------------------------
  simb::GeneratedParticleIndex_t ParticleListActionService::GetPrimaryTruthIndex(int trackId) const
  {
    auto const it = fPrimaryTruthMap.find(trackId);
    return it == fPrimaryTruthMap.end() ? simb::NoGeneratedParticleIndex : it->second;
  }

  //----------------------------------------------------------------------------
  // Yields the ParticleList accumulated during the current event.
  sim::ParticleList&& ParticleListActionService::YieldList()
  {
    // check if the ParticleNavigator has entries, and if
    // so grab the highest track id value from it to
    // add to the fTrackIDOffset
    int highestID = 0;
    for (auto pn = fParticleList.begin(); pn != fParticleList.end(); pn++)
      if ((*pn).first > highestID) highestID = (*pn).first;

    // If we have stored dropped particles,
    // include them in the offset.
    if (fdroppedParticleList) {
      for (auto pn = fdroppedParticleList->begin(); pn != fdroppedParticleList->end(); pn++)
        if ((*pn).first > highestID) highestID = (*pn).first;
    }

    //Only change the fTrackIDOffset if there is in fact a particle to add to the event
    if ((fParticleList.size()) != 0) {
      fTrackIDOffset = highestID + 1;
      mf::LogDebug("YieldList:fTrackIDOffset")
        << "highestID = " << highestID << "\nfTrackIDOffset= " << fTrackIDOffset;
    }

    return std::move(fParticleList);
  } // ParticleList&& ParticleListActionService::YieldList()
  //----------------------------------------------------------------------------
  // Yields the (dropped) ParticleList accumulated during the current event.
  sim::ParticleList&& ParticleListActionService::YieldDroppedList()
  {
    if (!fdroppedParticleList) {
      throw cet::exception("ParticleListAction")
        << "ParticleListAction::YieldDroppedList(): dropped particle list not build by user "
           "request.\n";
    }
    // check if the ParticleNavigator has entries, and if
    // so grab the highest track id value from it to
    // add to the fTrackIDOffset
    int highestID = 0;
    for (auto pn = fParticleList.begin(); pn != fParticleList.end(); pn++)
      if ((*pn).first > highestID) highestID = (*pn).first;

    // If we have stored dropped particles,
    // include them in the offset.
    for (auto pn = fdroppedParticleList->begin(); pn != fdroppedParticleList->end(); pn++)
      if ((*pn).first > highestID) highestID = (*pn).first;

    //Only change the fTrackIDOffset if there is in fact a particle to add to the event
    if ((fParticleList.size()) != 0) {
      fTrackIDOffset = highestID + 1;
      mf::LogDebug("YieldList:fTrackIDOffset")
        << "highestID = " << highestID << "\nfTrackIDOffset= " << fTrackIDOffset;
    }

    return std::move(*fdroppedParticleList);
  } // ParticleList&& ParticleListAction::YieldDroppedList()
  //----------------------------------------------------------------------------
  // Dropped particle test

  bool ParticleListActionService::isDropped(simb::MCParticle const* p)
  {
    return !p || p->Trajectory().empty();
  } // ParticleListAction::isDropped()
  //----------------------------------------------------------------------------
  void ParticleListActionService::AddPointToCurrentParticle(TLorentzVector const& pos,
                                                            TLorentzVector const& mom,
                                                            std::string const& process)
  {
    // Add the first point in the trajectory.
    fCurrentParticle.particle->AddTrajectoryPoint(pos, mom, process, fKeepTransportation);
  }

  // Called at the end of each event. Call detectors to convert hits for the
  // event and pass the call on to the action objects.
  void ParticleListActionService::endOfEventAction(const G4Event*)
  {
    // -- End of Run Report
    if (!fNotStoredCounterUMap.empty()) { // -- Only if there is something to report
      std::stringstream sscounter;
      sscounter << "Not Stored Process summary:";
      for (auto const& [process, count] : fNotStoredCounterUMap) {
        sscounter << "\n\t" << process << " : " << count;
      }
      mf::LogInfo("ParticleListActionService") << sscounter.str();
    }

    partCol_ = std::make_unique<std::vector<simb::MCParticle>>();
    droppedCol_ = std::make_unique<sim::ParticleAncestryMap>();
    droppedPartCol_ = std::make_unique<std::vector<sim::MCParticleLite>>();
    tpassn_ =
      std::make_unique<art::Assns<simb::MCTruth, simb::MCParticle, sim::GeneratedParticleInfo>>();
    // Set up the utility class for the "for_each" algorithm.  (We only
    // need a separate set-up for the utility class because we need to
    // give it the pointer to the particle list.  We're using the STL
    // "for_each" instead of the C++ "for loop" because it's supposed
    // to be faster.
    std::for_each(
      fParticleList.begin(), fParticleList.end(), UpdateDaughterInformation{fParticleList});

    MF_LOG_INFO("endOfEventAction") << "MCTruth Handles Size: " << fMCLists->size();

    unsigned int nGeneratedParticles = 0;
    unsigned int nMCTruths = 0;
    sim::ParticleList particleList = YieldList();
    // Request a list of dropped particles
    sim::ParticleList droppedParticleList;
    if (fdroppedParticleList) {droppedParticleList = YieldDroppedList();}    
    for (size_t mcl = 0; mcl < fMCLists->size(); ++mcl) {
      auto const& mclistHandle = (*fMCLists)[mcl];
      MF_LOG_INFO("endOfEventAction") << "mclistHandle Size: " << mclistHandle->size();
      for (size_t m = 0; m < mclistHandle->size(); ++m) {
        art::Ptr<simb::MCTruth> mct(mclistHandle, m);
        MF_LOG_INFO("endOfEventAction") << "Found " << mct->NParticles() << " particles";
        for (simb::MCParticle* p : particleList | ranges::views::values) {
          auto gen_index = fMCTIndexMap[p->TrackId()];
          if (gen_index != nMCTruths) continue;
          // if the particle has been marked as dropped, we don't save it
          // (as of LArSoft ~v5.6 this does not ever happen because
          // ParticleListAction has already taken care of deleting them)
          // if (isDropped(p)) continue;
          assert(p->NumberTrajectoryPoints() != 0ull);
          ++nGeneratedParticles;
          sim::GeneratedParticleInfo const truthInfo{GetPrimaryTruthIndex(p->TrackId())};
          if (!truthInfo.hasGeneratedParticleIndex() && p->Mother() == 0) {
            MF_LOG_WARNING("endOfEventAction") << "No GeneratedParticleIndex()!";
            // this means it's primary but with no information; logic error!!
            throw art::Exception(art::errors::LogicError)
              << "Failed to match primary particle:\n"
              << "\nwith particles from the truth record '" << mclistHandle.provenance()->inputTag()
              << "':\n\n";
          }
          partCol_->push_back(std::move(*p));
          art::Ptr<simb::MCParticle> mcp_ptr{pid_, partCol_->size() - 1, productGetter_};
          tpassn_->addSingle(mct, mcp_ptr, truthInfo);

        }// endfor p in particleList
        if (fStoreDroppedMCParticles && droppedPartCol_) {
          for (simb::MCParticle* p : droppedParticleList | ranges::views::values) {
            if (isDropped(p)) continue; //Is it dropped?? 
            if (p->StatusCode() != 1) continue; //Is it a primary particle??

            sim::MCParticleLite mini_mcp(*p);
            mini_mcp.Origin(mct->Origin());

            droppedPartCol_->push_back(std::move(mini_mcp));
          } // for(droppedParticleList)
        }// if (fStoreDroppedMCParticles && droppedPartCol_)
        mf::LogDebug("Offset") << "nGeneratedParticles = " << nGeneratedParticles;
        droppedCol_->SetMap(fdroppedTracksMap);
        ++nMCTruths;
      }
    }
    fTrackIDOffset = 0;
  }
} // namespace LArG4
