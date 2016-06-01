// Copyright 2009-2016 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2016, Sandia Corporation
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#include <sst_config.h>
#include <sst/core/params.h>
#include <sst/elements/miranda/generators/singlestream.h>

using namespace SST::Miranda;

SingleStreamGenerator::SingleStreamGenerator( Component* owner, Params& params ) :
	RequestGenerator(owner, params) {

	const uint32_t verbose = params.find<uint32_t>("verbose", 0);

	out = new Output("SingleStreamGenerator[@p:@l]: ", verbose, 0, Output::STDOUT);

	issueCount = params.find<uint64_t>("count", 1000);
	reqLength  = params.find<uint64_t>("length", 8);
	nextAddr   = params.find<uint64_t>("startat", 0);
	maxAddr    = params.find<uint64_t>("max_address", 524288);

	out->verbose(CALL_INFO, 1, 0, "Will issue %" PRIu64 " operations\n", issueCount);
	out->verbose(CALL_INFO, 1, 0, "Request lengths: %" PRIu64 " bytes\n", reqLength);
	out->verbose(CALL_INFO, 1, 0, "Maximum address: %" PRIu64 "\n", maxAddr);
	out->verbose(CALL_INFO, 1, 0, "First address: %" PRIu64 "\n", nextAddr);
}

SingleStreamGenerator::~SingleStreamGenerator() {
	delete out;
}

void SingleStreamGenerator::generate(MirandaRequestQueue<GeneratorRequest*>* q) {
	out->verbose(CALL_INFO, 4, 0, "Generating next request number: %" PRIu64 "\n", issueCount);

	q->push_back(new MemoryOpRequest(nextAddr, reqLength, READ));

	// What is the next address?
	nextAddr = (nextAddr + reqLength) % maxAddr;

	issueCount--;
}

bool SingleStreamGenerator::isFinished() {
	return (issueCount == 0);
}

void SingleStreamGenerator::completed() {

}
