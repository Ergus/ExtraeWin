import struct
import numpy as np
import os
import sys
import datetime as dt

# lEntry="i I L L"
# entry = struct.Struct(lEntry)


class ParsedTraces:
    '''Class with imported information from trace binary file.'''
    headerType = struct.Struct("I I")
    eventType = np.dtype([('id', 'i1'), ('value', 'u1'), ('core', 'u2'), ('tid', 'u4'), ('time', 'u4')])

    def __init__(self):
        self.traceDict = {}
        self.startEvent = None
        self.lastEvent = None
        self.nevents = 0
        self.threadList = []
        self.coresList = []

    def addTraceFile(self, trace_file_name):
        headerSize: int = ParsedTraces.headerType.size

        # Read only the header information before importing
        with open(trace_file_name, "rb") as traceFile:
            cpu, nevents \
                = ParsedTraces.headerType.unpack(traceFile.read(headerSize))

        # Now import the rest of the trace in a numpy array
        self.traceDict[cpu] \
            = np.fromfile(trace_file_name, ParsedTraces.eventType,
                          nevents, "", headerSize)

        # Now check the boundaries and expand them if needed
        first = self.traceDict[cpu][0]
        if not self.startEvent or first['time'] < self.startEvent['time']:
            self.startEvent = first.copy()

        last = self.traceDict[cpu][-1]
        if not self.lastEvent or last['time'] > self.lastEvent['time']:
            self.lastEvent = last.copy()

        # get the indices of the first unique tids
        indices \
            = np.unique(self.traceDict[cpu][:]['tid'], return_index=True)[1]

        tids = self.traceDict[cpu][sorted(indices)]['tid']
        self.threadList += [i for i in tids if i not in self.threadList]

    def _getHeaderLine(self):
        '''Print the Paraver Header'''
        # Paraver (dd/mm/yy at hh:mm):time:nNodes(nCpus1,...,nCpusN):nApps:app1[...]
        elapsed = self.lastEvent['time'] - self.startEvent['time']

        start = self.startEvent['time']/1000000
        date = dt.datetime.fromtimestamp(start).strftime('%d/%m/%Y at %H:%M')

        cores = len(self.traceDict)

        threads = len(self.threadList)

        return f"#Paraver ({date}):{elapsed}:1({cores}):1:1({threads}:1)"

    @staticmethod
    def _eventToStr(event):
        '''Print an event'''
        # type:cpu:app:task:thread:time:event:value
        return f"2:{event['core']}:1:1:{event['tid']}:{event['time']}:{event['id']}:{event['value']}"

    def _processEvent(self, input, tidMap):
        output = input.copy()
        output['core'] += 1
        output['time'] -= self.startEvent['time']
        output['tid'] = tidMap[input['tid']]

        return output

    @staticmethod
    def __mergeTwo(a, b):
        """Merge two trace containers respecting the order"""
        merged = np.zeros(len(a) + len(b), dtype=ParsedTraces.eventType)

        it = ita = itb = 0
        lena = len(a)
        lenb = len(b)

        while ita < lena and itb < lenb:
            if a[ita]['time'] < b[itb]["time"]:
                merged[it] = a[ita]
                ita += 1
            else:
                merged[it] = b[itb]
                itb += 1
            it += 1
        while ita < lena:
            merged[it] = a[ita]
            ita += 1
            it += 1
        while itb < lenb:
            merged[it] = b[itb]
            itb += 1
            it += 1

        return merged

    def _merge(self):
        """Create a contiguous list with all the events merged

        This merged the arrays by pairs in order to reduce the worst case merge
        conditions where the merge arrays grow too much."""
        traces = list(self.traceDict.values())

        merged = []
        while len(traces) > 1:
            merged = []
            for i in range(0, len(traces), 2):
                if len(traces) - i > 1:
                    merged.append(ParsedTraces.__mergeTwo(traces[i], traces[i + 1]))
                else:
                    merged.append(traces[i])
            traces = merged

        return merged[0]

    def __str__(self):
        '''Get the full trace'''
        merged = self._merge()

        tidMap = {v: k for k, v in enumerate(self.threadList)}

        ret = self._getHeaderLine() + "\n"
        for event in merged:
            if event['id'] >= 0:
                tmp = self._processEvent(event, tidMap)
                ret += ParsedTraces._eventToStr(tmp) + "\n"

        return ret


if __name__ == "__main__":
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise Exception(f"Wrong script argument. Usage: ./{sys.argv[0]} trace_directory")

    directory = sys.argv[1]

    traces = ParsedTraces()

    for tracefile in [f for f in os.listdir(directory) if f.endswith(".bin")]:
        traces.addTraceFile(os.path.join(directory, tracefile))

    out_file_name = os.path.join(directory, "Trace.prv")
    with open(out_file_name, "w") as ouputfile:
         print(traces, file=ouputfile)
