
#ifndef _H_VANADIS_MUL_SPLIT
#define _H_VANADIS_MUL_SPLIT

#include "inst/vinst.h"

namespace SST {
namespace Vanadis {

class VanadisMultiplySplitInstruction : public VanadisInstruction {
public:
	VanadisMultiplySplitInstruction(
		const uint64_t addr,
		const uint32_t hw_thr,
		const VanadisDecoderOptions* isa_opts,
		const uint16_t dest_lo,
		const uint16_t dest_hi,
		const uint16_t src_1,
		const uint16_t src_2,
		bool performSgnd ) :
		VanadisInstruction(addr, hw_thr, isa_opts, 2, 2, 2, 2, 0, 0, 0, 0),
			performSigned(performSgnd) {

		isa_int_regs_in[0]  = src_1;
		isa_int_regs_in[1]  = src_2;
		isa_int_regs_out[0] = dest_lo;
		isa_int_regs_out[1] = dest_hi;
	}

	VanadisMultiplySplitInstruction* clone() {
		return new VanadisMultiplySplitInstruction( *this );
	}

	virtual VanadisFunctionalUnitType getInstFuncType() const {
		return INST_INT_ARITH;
	}

	virtual const char* getInstCode() const {
		return "MULSPLIT";
	}

	virtual void printToBuffer(char* buffer, size_t buffer_size) {
                snprintf(buffer, buffer_size, "MULSPLIT lo: %5" PRIu16 " hi: %" PRIu16 " <- %5" PRIu16 " * %5" PRIu16 " (phys: lo: %5" PRIu16 " hi: %" PRIu16 " <- %5" PRIu16 " * %5" PRIu16 ")",
			isa_int_regs_out[0], isa_int_regs_out[1],
			isa_int_regs_in[0], isa_int_regs_in[1],
			phys_int_regs_out[0], phys_int_regs_out[1],
			phys_int_regs_in[0], phys_int_regs_in[1] );
        }

	virtual void execute( SST::Output* output, VanadisRegisterFile* regFile ) {
		output->verbose(CALL_INFO, 16, 0, "Execute: (addr=%p) MULSPLIT phys: out-lo: %" PRIu16 " out-hi: %" PRIu16 " in=%" PRIu16 ", %" PRIu16 ", isa: out-lo: %" PRIu16 " out-hi: %" PRIu16 " / in=%" PRIu16 ", %" PRIu16 "\n",
			(void*) getInstructionAddress(), phys_int_regs_out[0], phys_int_regs_out[1],
			phys_int_regs_in[0], phys_int_regs_in[1],
			isa_int_regs_out[0], isa_int_regs_out[1], isa_int_regs_in[0], isa_int_regs_in[1] );

		if( performSigned ) {
			const int64_t src_1 = regFile->getIntReg<int64_t>( phys_int_regs_in[0] );
			const int64_t src_2 = regFile->getIntReg<int64_t>( phys_int_regs_in[1] );

			const int64_t multiply_result = (src_1) * (src_2);
			const int64_t result_lo       = (int32_t)( multiply_result & 0xFFFFFFFF );
			const int64_t result_hi       = (int32_t)( multiply_result >> 32 );

			output->verbose(CALL_INFO, 16, 0, "-> Execute: (detail, signed, MULSPLIT) %" PRId64 " * %" PRId64 " = %" PRId64 " = (lo: %" PRId64 ", hi: %" PRId64 " )\n",
				src_1, src_2, multiply_result, result_lo, result_hi );

			regFile->setIntReg( phys_int_regs_out[0], result_lo );
			regFile->setIntReg( phys_int_regs_out[1], result_hi );
		} else {
			const uint64_t src_1 = regFile->getIntReg<uint64_t>( phys_int_regs_in[0] );
			const uint64_t src_2 = regFile->getIntReg<uint64_t>( phys_int_regs_in[1] );

			const uint64_t multiply_result = (src_1) * (src_2);
			const uint64_t result_lo       = (uint32_t)( multiply_result & 0xFFFFFFFF );
			const uint64_t result_hi       = (uint32_t)( multiply_result >> 32 );

			output->verbose(CALL_INFO, 16, 0, "-> Execute: (detail, unsigned, MULSPLIT) %" PRIu64 " * %" PRIu64 " = %" PRIu64 " = (lo: %" PRIu64 ", hi: %" PRIu64 " )\n",
				src_1, src_2, multiply_result, result_lo, result_hi );

			regFile->setIntReg( phys_int_regs_out[0], result_lo );
			regFile->setIntReg( phys_int_regs_out[1], result_hi );
		}
		markExecuted();
	}

protected:
	bool performSigned;

};

}
}

#endif
