
#ifndef _H_VANADIS_FP_SET_REG_COMPARE
#define _H_VANADIS_FP_SET_REG_COMPARE

#include "inst/vinst.h"
#include "inst/vcmptype.h"

#include "util/vfpreghandler.h"

#define VANADIS_MIPS_FP_COMPARE_BIT 0x800000
#define VANADIS_MIPS_FP_COMPARE_BIT_INVERSE 0xFF7FFFFF

namespace SST {
namespace Vanadis {

class VanadisFPSetRegCompareInstruction : public VanadisInstruction {
public:
	VanadisFPSetRegCompareInstruction(
                const uint64_t addr,
                const uint32_t hw_thr,
                const VanadisDecoderOptions* isa_opts,
		const uint16_t dest,
		const uint16_t src_1,
		const uint16_t src_2,
		const VanadisFPRegisterFormat r_fmt,
		const VanadisRegisterCompareType cType
		) :
		VanadisInstruction(addr, hw_thr, isa_opts, 0, 0, 0, 0,
			( (r_fmt == VANADIS_FORMAT_FP64) && ( VANADIS_REGISTER_MODE_FP32 == isa_opts->getFPRegisterMode() ) ) ? 5 : 3, 1,
			( (r_fmt == VANADIS_FORMAT_FP64) && ( VANADIS_REGISTER_MODE_FP32 == isa_opts->getFPRegisterMode() ) ) ? 5 : 3, 1),
			reg_fmt(r_fmt), compareType(cType) {

		if( (r_fmt == VANADIS_FORMAT_FP64) && ( VANADIS_REGISTER_MODE_FP32 == isa_opts->getFPRegisterMode() ) ) {
			isa_fp_regs_in[0]  = src_1;
			isa_fp_regs_in[1]  = src_1 + 1;
			isa_fp_regs_in[2]  = src_2;
			isa_fp_regs_in[3]  = src_2 + 1;
			isa_fp_regs_in[4]  = dest;
			isa_fp_regs_out[0] = dest;
		} else {
			isa_fp_regs_in[0]  = src_1;
			isa_fp_regs_in[1]  = src_2;
			isa_fp_regs_in[2]  = dest;
			isa_fp_regs_out[0] = dest;
		}
	}

	VanadisFPSetRegCompareInstruction* clone() {
		return new VanadisFPSetRegCompareInstruction( *this );
	}

	virtual VanadisFunctionalUnitType getInstFuncType() const {
		return INST_FP_ARITH;
	}

	virtual const char* getInstCode() const {
		switch( reg_fmt ) {
		case VANADIS_FORMAT_FP64:
			return "FP64CMP";
		case VANADIS_FORMAT_FP32:
			return "FP32CMP";
		case VANADIS_FORMAT_INT64:
			return "FPINT64ACMP";
		case VANADIS_FORMAT_INT32:
			return "FPINT32CMP";
		}
	}

	virtual void printToBuffer(char* buffer, size_t buffer_size ) {
		snprintf( buffer, buffer_size, "FPCMPST (op: %s, %s) isa-out: %" PRIu16 " isa-in: %" PRIu16 ", %" PRIu16 " / phys-out: %" PRIu16 " phys-in: %" PRIu16 ", %" PRIu16 "\n",
			convertCompareTypeToString(compareType),
			registerFormatToString(reg_fmt),
			isa_fp_regs_out[0], isa_fp_regs_in[0], isa_fp_regs_in[1],
			phys_fp_regs_out[0], phys_fp_regs_in[0], phys_fp_regs_in[1]);
	}

	template<typename T>
	bool performCompare( SST::Output* output, VanadisRegisterFile* regFile ) {
		const T left_value  = ( (8 == sizeof(T)) && (VANADIS_REGISTER_MODE_FP32 == isa_options->getFPRegisterMode()) ) ?
			combineFromRegisters<T>( regFile, phys_fp_regs_in[0], phys_fp_regs_in[1] ) : regFile->getFPReg<T>( phys_fp_regs_in[0] );
		const T right_value = ( (8 == sizeof(T)) && (VANADIS_REGISTER_MODE_FP32 == isa_options->getFPRegisterMode()) ) ?
                        combineFromRegisters<T>( regFile, phys_fp_regs_in[2], phys_fp_regs_in[3] ) : regFile->getFPReg<T>( phys_fp_regs_in[1] );

		switch( compareType ) {
		case REG_COMPARE_EQ:
			return (left_value == right_value);
		case REG_COMPARE_NEQ:
			return (left_value != right_value);
		case REG_COMPARE_LT:
			return (left_value < right_value);
		case REG_COMPARE_LTE:
			return (left_value <= right_value);
		case REG_COMPARE_GT:
			return (left_value > right_value);
		case REG_COMPARE_GTE:
			return (left_value >= right_value);
		default:
			output->fatal(CALL_INFO, -1, "Unknown compare type.\n");
			return false;
		}
	}

	virtual void execute( SST::Output* output, VanadisRegisterFile* regFile ) {
		char* int_register_buffer = new char[256];
		char* fp_register_buffer = new char[256];

		writeIntRegs( int_register_buffer, 256 );
		writeFPRegs(  fp_register_buffer,  256 );

		output->verbose(CALL_INFO, 16, 0, "Execute: (addr=0x%llx) %s (%s) int: %s / fp: %s\n",
			getInstructionAddress(), getInstCode(),
			convertCompareTypeToString(compareType),
			int_register_buffer, fp_register_buffer);

		delete[] int_register_buffer;
		delete[] fp_register_buffer;

		bool compare_result = false;
		bool byte8_type     = false;

		switch( reg_fmt ) {
		case VANADIS_FORMAT_FP32:
			compare_result = performCompare<float>( output, regFile );
			break;
		case VANADIS_FORMAT_FP64:
			compare_result = performCompare<double>( output, regFile );
			byte8_type = true;
			break;
		case VANADIS_FORMAT_INT32:
			compare_result = performCompare<int32_t>( output, regFile );
			break;
		case VANADIS_FORMAT_INT64:
			compare_result = performCompare<int64_t>( output, regFile );
			byte8_type = true;
			break;
		default:
			output->fatal(CALL_INFO, -1, "Unknown data format type.\n");
		}

		const uint16_t cond_reg_in  = byte8_type ? phys_fp_regs_in[4] : phys_fp_regs_in[2];
		const uint16_t cond_reg_out = phys_fp_regs_out[0];

		uint32_t cond_val = (regFile->getFPReg<uint32_t>( cond_reg_in ) & VANADIS_MIPS_FP_COMPARE_BIT_INVERSE);

		if( compare_result ) {
			// true, keep everything else the same and set the compare bit to 1
			cond_val = (cond_val | VANADIS_MIPS_FP_COMPARE_BIT);
			output->verbose(CALL_INFO, 16, 0, "---> result: true\n");
		} else {
			output->verbose(CALL_INFO, 16, 0, "---> result: false\n");
		}

		regFile->setFPReg<uint32_t>( cond_reg_out, cond_val );

		markExecuted();
	}

protected:
	VanadisFPRegisterFormat reg_fmt;
	VanadisRegisterCompareType compareType;

};

}
}

#endif
