#include <fstream>
#include <sstream>
#include <iterator>

#include "llvm/Support/CommandLine.h"

// FIXME: Ugh, this is gross. But otherwise our config.h conflicts with LLVMs.
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#include "klee/Init.h"
#include "klee/Expr.h"
#include "klee/ExprBuilder.h"
#include <klee/Solver.h>
#include "../../lib/Core/Memory.h"

#include "spa/CFG.h"
#include "spa/CG.h"
#include "spa/SPA.h"
#include "spa/Path.h"
#include "spa/CFGBackwardFilter.h"
#include "spa/WhitelistIF.h"
#include "spa/DummyIF.h"
#include "spa/NegatedIF.h"
#include "spa/WaypointUtility.h"
#include "spa/AstarUtility.h"
#include "spa/FilteredUtility.h"
#include "spa/PathFilter.h"

extern std::string InputFile;

namespace {
	llvm::cl::opt<std::string> DumpCFG( "dump-cfg",
		llvm::cl::desc( "Dumps the analyzed program's annotated CFG to the given file, as a .dot file." ) );

	llvm::cl::opt<std::string> DumpCG( "dump-cg",
		llvm::cl::desc( "Dumps the analyzed program's CG to the given file, as a .dot file." ) );

// 	llvm::cl::opt<bool> SaveSeeds( "save-seeds",
// 		llvm::cl::desc( "Generates seed paths from seed inputs." ) );
// 
// 	llvm::cl::opt<std::string> SeedFile( "seed-file",
// 		llvm::cl::desc( "Loads previously generated seed paths." ) );

	llvm::cl::opt<std::string> InitValueFile( "init-values",
		llvm::cl::desc( "Loads initial values (typically state) from the specified file." ) );

	llvm::cl::opt<std::string> PathFile( "path-file",
		llvm::cl::desc( "Sets the output path file." ) );

	llvm::cl::opt<bool> Client( "client",
		llvm::cl::desc( "Explores client paths (API to packet output)." ),
		cl::init( false ) );

	llvm::cl::opt<bool> Server( "server",
		llvm::cl::desc( "Explores server paths (packet input to API)." ) );
}

class SpaClientPathFilter : public SPA::PathFilter {
public:
	bool checkPath( SPA::Path &path ) {
		return path.getTag( SPA_HANDLERTYPE_TAG ) == SPA_APIHANDLER_VALUE &&
			path.getTag( SPA_OUTPUT_TAG ) == SPA_OUTPUT_VALUE /*&&
			path.getTag( "ReplayDone" ) == "1"*/;
	}
};

class SpaServerPathFilter : public SPA::PathFilter {
public:
	bool checkPath( SPA::Path &path ) {
		return path.getTag( SPA_HANDLERTYPE_TAG ) == SPA_MESSAGEHANDLER_VALUE &&
			path.getTag( SPA_VALIDPATH_TAG ) != SPA_VALIDPATH_VALUE;
	}
};

int main(int argc, char **argv, char **envp) {
	// Fill up every global cl::opt object declared in the program
	cl::ParseCommandLineOptions( argc, argv, "Systematic Protocol Analyzer" );

	assert( ((! Client) != (! Server)) && "Must specify either --client or --server." );

	llvm::Module *module = klee::loadByteCode();
	module = klee::prepareModule( module );

	std::string pathFileName = PathFile;
	if ( pathFileName == "" )
		pathFileName = InputFile + (Client ? ".client" : ".server") + ".paths";
	CLOUD9_INFO( "Writing output to: " << pathFileName );
	std::ofstream pathFile( pathFileName.c_str(), std::ios::out | std::ios::app );
	assert( pathFile.is_open() && "Unable to open path file." );
	SPA::SPA spa = SPA::SPA( module, pathFile );

	// Pre-process the CFG and select useful paths.
	CLOUD9_INFO( "Pruning CFG." );

	// Get full CFG and call-graph.
	SPA::CFG cfg( module );
	SPA::CG cg( cfg );

	if ( InitValueFile.size() > 0 ) {
		CLOUD9_DEBUG( "   Setting up initial values." );
		Function *fn = module->getFunction( SPA_INPUT_ANNOTATION_FUNCTION );
		assert( fn );
		std::map<std::string,std::pair<llvm::Value *,size_t> > initValueVars;
		for ( std::set<llvm::Instruction *>::iterator it = cg.getDefiniteCallers( fn ).begin(), ie = cg.getDefiniteCallers( fn ).end(); it != ie; it++ ) {
			const CallInst *callInst;
			assert( callInst = dyn_cast<CallInst>( *it ) );
			assert( callInst->getNumArgOperands() == 5 );
			const llvm::ConstantInt *ci;
			assert( ci = dyn_cast<llvm::ConstantInt>( callInst->getArgOperand( 1 ) ) );
			uint64_t size = ci->getValue().getLimitedValue();
			llvm::User *u;
			assert( u = dyn_cast<User>( callInst->getArgOperand( 2 ) ) );
			llvm::GlobalVariable *gv;
			assert( gv = dyn_cast<GlobalVariable>( u->getOperand( 0 ) ) );
			llvm::ConstantArray *ca;
			assert( ca = dyn_cast<ConstantArray>( gv->getInitializer() ) );
			// string reconversion to fix LLVM bug (includes null in std::string).
			std::string name = ca->getAsString().c_str();
			llvm::Value *var = callInst->getArgOperand( 3 );

			CLOUD9_INFO( "      Found input " << name << "[" << size << "]." );
			assert( initValueVars.count( name ) == 0 && "Input multiply declared.");
			initValueVars[name] = std::pair<llvm::Value *,size_t>( var, size );
		}

		std::ifstream initValueFile( InitValueFile.c_str() );
		assert( initValueFile.is_open() );
		std::map<llvm::Value *, std::vector<std::vector<std::pair<bool,uint8_t> > > > initValues;
		while ( initValueFile.good() ) {
			std::string line;
			getline( initValueFile, line );
			if ( ! line.empty() ) {
				std::string name;
				std::vector<std::pair<bool,uint8_t> > value;
				std::stringstream ss( line );
				ss >> name;
				ss << std::hex;
				while ( ss.good() ) {
					int v;
					ss >> v;
					if ( ! ss.fail() ) {
						value.push_back( std::pair<bool,uint8_t>( true, v ) );
					} else {
						value.push_back( std::pair<bool,uint8_t>( false, 0 ) );
						ss.clear();
						std::string dummy;
						ss >> dummy;
					}
				}

				CLOUD9_DEBUG( "      Found initial value for " << name << "[" << value.size() << "]" << "." );
				assert( initValueVars.count( name ) > 0 && "Initial value defined but not used." );
				assert( initValueVars[name].second >= value.size() && "Initial value doesn't fit in variable." );

				// If last specified by is concrete pad value with concrete 0s to fill variable, otherwise pad with symbols.
				for ( size_t i = value.size(); i < initValueVars[name].second; i++ )
					value.push_back( std::pair<bool,uint8_t>( value.back().first, 0 ) );

				initValues[initValueVars[name].first].push_back( value );
			} else {
				if ( ! initValues.empty() ) {
					CLOUD9_INFO( "      Adding set of " << initValues.size() << " initial values." );
					spa.addInitialValues( initValues );
					initValues.clear();
				}
			}
		}
		if ( ! initValues.empty() ) {
			CLOUD9_INFO( "      Adding initial value set." );
			spa.addInitialValues( initValues );
		}
	} else {
		CLOUD9_INFO( "      No initial input values given, leaving symbolic." );
		spa.addSymbolicInitialValues();
	}

// 	// Find seed IDs.
// 	std::set<unsigned int> seedIDs;
// 	if ( SaveSeeds ) {
// 		CLOUD9_DEBUG( "   Setting up path seed generation." );
// 		Function *fn = module->getFunction( SPA_SEED_ANNOTATION_FUNCTION );
// 		assert( fn );
// 		for ( std::set<llvm::Instruction *>::iterator it = cg.getDefiniteCallers( fn ).begin(), ie = cg.getDefiniteCallers( fn ).end(); it != ie; it++ ) {
// 			const CallInst *callInst;
// 			assert( callInst = dyn_cast<CallInst>( *it ) );
// 			assert( callInst->getNumArgOperands() == 4 );
// 			const llvm::ConstantInt *constInt;
// 			assert( constInt = dyn_cast<llvm::ConstantInt>( callInst->getArgOperand( 0 ) ) );
// 			uint64_t id = constInt->getValue().getLimitedValue();
// 			if ( ! seedIDs.count( id ) )
// 				CLOUD9_INFO( "      Found seed id: " << id );
// 			seedIDs.insert( id );
// 		}
// 		assert( ! seedIDs.empty() );
// 	}

	// Find entry handlers.
	std::set<llvm::Instruction *> entryPoints;
	if ( Client ) {
		Function *fn = module->getFunction( SPA_API_ANNOTATION_FUNCTION );
		if ( fn ) {
			std::set<llvm::Instruction *> apiCallers = cg.getDefiniteCallers( fn );
			for ( std::set<llvm::Instruction *>::iterator cit = apiCallers.begin(), cie = apiCallers.end(); cit != cie; cit++ ) {
				CLOUD9_DEBUG( "   Found API entry function: " << (*cit)->getParent()->getParent()->getName().str() );
// 				if ( seedIDs.empty() )
					spa.addEntryFunction( (*cit)->getParent()->getParent() );
// 				else
// 					for ( std::set<unsigned int>::iterator sit = seedIDs.begin(), sie = seedIDs.end(); sit != sie; sit++ )
// 						spa.addSeedEntryFunction( *sit, (*cit)->getParent()->getParent() );
				entryPoints.insert( *cit );
			}
		} else {
			CLOUD9_INFO( "   API annotation function not present in module." );
		}
	} else if ( Server ) {
		Function *fn = module->getFunction( SPA_MESSAGE_HANDLER_ANNOTATION_FUNCTION );
		if ( fn ) {
			std::set<llvm::Instruction *> mhCallers = cg.getDefiniteCallers( fn );
			for ( std::set<llvm::Instruction *>::iterator cit = mhCallers.begin(), cie = mhCallers.end(); cit != cie; cit++ ) {
				CLOUD9_DEBUG( "   Found message handler entry function: " << (*cit)->getParent()->getParent()->getName().str() );
// 				if ( seedIDs.empty() )
					spa.addEntryFunction( (*cit)->getParent()->getParent() );
// 				else
// 					for ( std::set<unsigned int>::iterator sit = seedIDs.begin(), sie = seedIDs.end(); sit != sie; sit++ )
// 						spa.addSeedEntryFunction( *sit, (*cit)->getParent()->getParent() );
				entryPoints.insert( *cit );
			}
		} else {
			CLOUD9_INFO( "   Message handler annotation function not present in module." );
		}
	}
	assert( (! entryPoints.empty()) && "No APIs or message handlers found." );

	// Rebuild full CFG and call-graph (changed by SPA after adding init/entry handlers).
	cfg = SPA::CFG( module );
	cg = SPA::CG( cfg );

	if ( DumpCG.size() > 0 ) {
		CLOUD9_DEBUG( "Dumping CG to: " << DumpCG.getValue() );
		std::ofstream dotFile( DumpCG.getValue().c_str() );
		assert( dotFile.is_open() && "Unable to open dump file." );

		cg.dump( dotFile );

		dotFile.flush();
		dotFile.close();
		return 0;
	}

	// Find checkpoints.
	CLOUD9_DEBUG( "   Setting up path checkpoints." );
	Function *fn = module->getFunction( SPA_CHECKPOINT_ANNOTATION_FUNCTION );
	std::set<llvm::Instruction *> checkpoints;
	if ( fn )
		checkpoints = cg.getDefiniteCallers( fn );
	else
		CLOUD9_INFO( "   Checkpoint annotation function not present in module." );
	assert( ! checkpoints.empty() && "No checkpoints found." );

	CLOUD9_DEBUG( "   Setting up path waypoints." );
	fn = module->getFunction( SPA_WAYPOINT_ANNOTATION_FUNCTION );
	std::map<unsigned int, std::set<llvm::Instruction *> > waypoints;
	if ( fn ) {
		for ( std::set<llvm::Instruction *>::iterator it = cg.getDefiniteCallers( fn ).begin(), ie = cg.getDefiniteCallers( fn ).end(); it != ie; it++ ) {
			const CallInst *callInst;
			CLOUD9_INFO( "Found waypoint in function: " << (*it)->getParent()->getParent()->getName().str() );
			assert( callInst = dyn_cast<CallInst>( *it ) );
			if ( callInst->getNumArgOperands() != 1 ) {
				CLOUD9_DEBUG( "Arguments: " << callInst->getNumArgOperands() );
				callInst->dump();
				assert( false && "Waypoint annotation function has wrong number of arguments." );
			}
			const llvm::ConstantInt *constInt;
			assert( constInt = dyn_cast<llvm::ConstantInt>( callInst->getArgOperand( 0 ) ) );
			uint64_t id = constInt->getValue().getLimitedValue();
			CLOUD9_INFO( "      Found waypoint with id " << id << " at " << (*it)->getParent()->getParent()->getName().str() << ":" << (*it)->getDebugLoc().getLine() );
			waypoints[id].insert( *it );
		}
	} else {
		CLOUD9_INFO( "   Waypoint annotation function not present in module." );
	}

	for ( std::set<llvm::Instruction *>::iterator it = checkpoints.begin(), ie = checkpoints.end(); it != ie; it++ )
		spa.addCheckpoint( *it );

	// Create instruction filter.
	CLOUD9_DEBUG( "   Creating CFG filter." );
	SPA::CFGBackwardFilter *filter = new SPA::CFGBackwardFilter( cfg, cg, checkpoints );
	if ( Client )
		spa.addStateUtilityBack( filter, false );
	else if ( Server )
		spa.addStateUtilityBack( filter, true );
	for ( std::set<llvm::Instruction *>::iterator it = entryPoints.begin(), ie = entryPoints.end(); it != ie; it++ ) {
		if ( ! filter->checkInstruction( *it ) ) {
			CLOUD9_DEBUG( "Entry point at function " << (*it)->getParent()->getParent()->getName().str() << " is not included in filter." );
// 			assert( false && "Entry point is filtered out." );
		}
	}

	// Create waypoint utility.
	SPA::WaypointUtility *waypointUtility = NULL;
	if ( ! waypoints.empty() ) {
		CLOUD9_DEBUG( "   Creating waypoint utility." );
		waypointUtility = new SPA::WaypointUtility( cfg, cg, waypoints, true );
		spa.addStateUtilityBack( waypointUtility, false );
	}

	// Create state utility function.
	CLOUD9_DEBUG( "   Creating state utility function." );
	
	spa.addStateUtilityBack( new SPA::FilteredUtility(), false );
	if ( Client ) {
		spa.addStateUtilityBack( new SPA::AstarUtility( module, cfg, cg, checkpoints ), false );
		spa.addStateUtilityBack( new SPA::TargetDistanceUtility( module, cfg, cg, checkpoints ), false );
	} else if ( Server && filter ) {
		spa.addStateUtilityBack( new SPA::AstarUtility( module, cfg, cg, *filter ), false );
		spa.addStateUtilityBack( new SPA::TargetDistanceUtility( module, cfg, cg, *filter ), false );
	}
	// All else being the same, go DFS.
	spa.addStateUtilityBack( new SPA::DepthUtility(), false );

	if ( DumpCFG.size() > 0 ) {
		CLOUD9_DEBUG( "Dumping CFG to: " << DumpCFG.getValue() );
		std::ofstream dotFile( DumpCFG.getValue().c_str() );
		assert( dotFile.is_open() && "Unable to open dump file." );

		std::map<SPA::InstructionFilter *, std::string> annotations;
		annotations[new SPA::WhitelistIF( checkpoints )] = "style = \"filled\" fillcolor = \"red\"";
// 		if ( filter )
// 			annotations[new SPA::NegatedIF( filter )] = "style = \"filled\" fillcolor = \"grey\"";

		cfg.dump( dotFile, /*filter*/ NULL, annotations, /*utility*/ /*waypointUtility*/ /*filter*/ /*NULL*/ new SPA::TargetDistanceUtility( module, cfg, cg, checkpoints ), false /*true*/ );

		dotFile.flush();
		dotFile.close();
		return 0;
	}

	if ( Client ) {
		spa.setOutputTerminalPaths( true );
		spa.setPathFilter( new SpaClientPathFilter() );
	} else if ( Server ) {
		spa.setOutputTerminalPaths( true );
		spa.setPathFilter( new SpaServerPathFilter() );
	}


	CLOUD9_DEBUG( "Starting SPA." );
	spa.start();

	pathFile.flush();
	pathFile.close();
	CLOUD9_DEBUG( "Done." );

	return 0;
}
