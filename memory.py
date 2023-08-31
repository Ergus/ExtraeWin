import sys
import matplotlib.pyplot as plt

def main(argv):
    if len(argv) == 1:
        sys.exit(f"Usage ./{argv[0]} Tracefile.prv")

    cum = 0
    arrayX = []
    arrayY = []

    with open(argv[1], "r") as inputfile:
        for line in inputfile:
            if len(line) == 0 or line[0] == "#":
                continue

            sline: str = line.split(":")

            if len(sline) == 8 and (sline[6] == "32769" or sline[6] == "32770"):

                if sline[6] == "32769":
                    diff = int(sline[7])
                elif sline[6] == "32770":
                    diff = -int(sline[7])

                cum += diff
                arrayX.append(int(sline[5]))
                arrayY.append(cum/1024)
                #print(f"{sline[5]},{diff},{cum}")

    fig, ax = plt.subplots()
    ax.step(arrayX, arrayY)
    ax.set(xlabel='Time (nS)', ylabel='Memory (kb)',
           title='Memory usage')
    ax.grid()

    fig.savefig("memory.png")
    plt.show()


if __name__ == "__main__":
    main(sys.argv)
