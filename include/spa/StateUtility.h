/*
 * SPA - Systematic Protocol Analysis Framework
 */

#ifndef __StateUtility_H__
#define __StateUtility_H__

namespace SPA {
	class StateUtility;
}

#include "klee/ExecutionState.h"
#include "spa/CFG.h"
#include "spa/CG.h"

namespace SPA {
	class StateUtility {
	public:
		/**
		 * Provides a utility metric to a given execution state, allowing some states to be explored earlier by priority.
		 * 
		 * @param state The state to check.
		 * @return The utility metric for the given state (higher = more useful).
		 */
		virtual double getUtility( const klee::ExecutionState *state ) = 0;

		/**
		 * Gives a Graphviz color for a given node to aid in debugging.
		 */
		virtual std::string getColor( CFG &cfg, CG &cg, llvm::Instruction *instruction ) { return "white"; }

	protected:
		virtual ~StateUtility() { }
	};
}

#endif // #ifndef __StateUtility_H__
