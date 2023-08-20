import sys

def main(argv):
    if len(argv) == 1:
        sys.exit(f"Usage ./{argv[0]} Tracefile.prv")

    cum = 0

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
                print(f"{sline[5]},{diff},{cum}")


if __name__ == "__main__":
    main(sys.argv)
