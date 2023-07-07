import struct
import numpy as np
import os
import sys
import datetime as dt

# lEntry="i I L L"
# entry = struct.Struct(lEntry)


class ParsedTraces:
    '''Class with imported information from trace binary file.'''
    headerType = struct.Struct("I I Q Q")

    eventType = np.dtype([('time', 'u8'),
                          ('id', 'u2'),
                          ('value', 'u2'),
                          ('core', 'u2'),
                          ('tid', 'u2')])

    def __init__(self):
        self.traceDict = {}
        self.startGlobalTime = 0
        self.startEvent = None
        self.lastEvent = None
        self.nevents = 0
        self.threadList = []
        self.coresList = []

    def addTraceFile(self, trace_file_name):
        '''Parse a trace file and load it into memory.'''
        headerSize: int = ParsedTraces.headerType.size

        # Read only the header information before importing
        with open(trace_file_name, "rb") as traceFile:
            id, nevents, tid, startGTime \
                = ParsedTraces.headerType.unpack(traceFile.read(headerSize))

        # Now import the rest of the trace in a numpy array
        self.traceDict[id] \
            = np.fromfile(trace_file_name,
                          ParsedTraces.eventType,
                          nevents,
                          "",
                          headerSize)

        # Now check the boundaries on the first thread
        if (id == 1):
            self.startEvent = self.traceDict[1][0].copy()
            self.lastEvent = self.traceDict[1][-1].copy()
            self.startGlobalTime = startGTime

        self.threadList.append(id)

        # get the indices of the first unique cores
        indices \
            = np.unique(self.traceDict[id][:]['core'], return_index=True)[1]

        cores = self.traceDict[id][sorted(indices)]['core']

        self.coresList += [i for i in cores if i not in self.coresList]

    def _getHeaderLine(self):
        '''Get the Paraver formatted Header'''
        # Paraver (dd/mm/yy at hh:mm):time:nNodes(nCpus1,...,nCpusN):nApps:app1[...]
        assert self.startGlobalTime != 0, "No main thread (id == 1) info set"

        elapsed = self.lastEvent['time'] - self.startEvent['time']

        start = self.startGlobalTime/1000000
        date = dt.datetime.fromtimestamp(start).strftime('%d/%m/%Y at %H:%M')

        cores = len(self.coresList)

        threads = len(self.threadList)

        return f"#Paraver ({date}):{elapsed}:1({cores}):1:1({threads}:1)"

    @staticmethod
    def __mergeTwo(a, b):
        """Merge two trace containers respecting the order

        This is usually the last step in a merge sort code, I am
        actually surprised that numpy does not provide such feature.
        """
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
        conditions where the merge arrays grow too much.
        """
        assert self.startEvent  # if this fails there was not thread zero file

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

    def _eventToStr(self, event):
        '''Print an event'''
        # type:cpu:app:task:thread:time:event:value
        return f"2:{event['core']}:1:1:{event['tid']}:{event['time'] - self.startEvent['time']}:{event['id']}:{event['value']}"

    def __str__(self):
        '''Get the full trace'''
        merged = self._merge()

        ret = self._getHeaderLine() + "\n"
        for event in merged:
            ret += self._eventToStr(event) + "\n"

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
