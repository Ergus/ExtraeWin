import struct
from collections import namedtuple
import numpy as np
import os
import sys
from operator import attrgetter

# lEntry="i I L L"
# entry = struct.Struct(lEntry)

class ParsedTraces:
    '''Class with imported information from trace binary file.'''
    header = struct.Struct("I N")
    eventType = np.dtype([('id', 'i4'), ('value', 'u4'), ('tid', 'u8'), ('time', 'u8')])

    def __init__(self):
        self.traceDict = {}

    def addTraceFile(self, trace_file_name):
        headerSize = ParsedTraces.header.size

        # Read only the header information before importing
        with open(trace_file_name, "rb") as traceFile:
            cpu, nevents = ParsedTraces.header.unpack(traceFile.read(headerSize))

        with open(trace_file_name, "rb") as traceFile:
            print(ParsedTraces.header.unpack(traceFile.read(headerSize)))


        # Now import the rest of the trace in a numpy array
        self.traceDict[cpu] \
            = np.fromfile(trace_file_name, ParsedTraces.eventType,
                          nevents, "", headerSize)

    def startEvent(self):
        '''Return the very first event'''
        return min([value[0] for value in self.traceDict.values()], key=attrgetter('time'))

    def lastEvent(self):
        '''Return the very last event'''
        return max([value[-1] for value in self.traceDict.values()], key=attrgetter('time'))

    def _printHeader(self):
        startEvent = self.startEvent()
        lastEvent = self.lastEvent()


#Paraver (dd/mm/yy at hh:mm):ftime:nNodes(nCpus1,...,nCpusN):nAppl:applicationList[:applicationLis

if __name__ == "__main__":
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise CustomException(f"Wrong script argument. Usage: ./{sys.argv[0]} trace_directory")

    directory = sys.argv[1]

    traces = ParsedTraces()

    for tracefile in [f for f in os.listdir(directory) if f.endswith(".bin")]:
        traces.addTraceFile(os.path.join(directory, tracefile))

    print(traces.traceDict)
