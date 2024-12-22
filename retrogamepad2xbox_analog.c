#include "retrogamepad2xbox.h"

void createAnalogSensitvityCSV() {
    //------------ -5% sensitivity
    FILE *file;
    int exists = 0;
    char *filepath = "/data/rgp2xbox/DecreaseAnalogSensitivityBy15Percent.csv";

    // Check if file exists
    file = fopen(filepath, "r");
    if (file) {
        exists = 1;
        fclose(file);
    }

    // Create file if it does not exist
    if (!exists) {
        file = fopen(filepath, "w");
        if (file == NULL) {
            perror("Error opening file");
            return;
        }

        // Set file permissions to 0666
        chmod(filepath, 0666);

        // Write data to CSV
        for (int i = -1800; i <= 1800; i++) {
            double secondValue;
            if (i == 0) {
                secondValue = 0;
            } else if (i > 0) {
                secondValue = ceil(i * 0.85);
            } else {
                secondValue = floor(i * 0.85);
            }
            fprintf(file, "%d,%.0f\n", i, secondValue);
        }
        fclose(file);
    }

    //------------ -10% sensitivity
    exists = 0;
    filepath = "/data/rgp2xbox/DecreaseAnalogSensitivityBy25Percent.csv";

    file = fopen(filepath, "r");
    if (file) {
        exists = 1;
        fclose(file);
    }
    if (!exists) {
        file = fopen(filepath, "w");
        if (file == NULL) {
            perror("Error opening file");
            return;
        }
        chmod(filepath, 0666);

        for (int i = -1800; i <= 1800; i++) {
            double secondValue;
            if (i == 0) {
                secondValue = 0;
            } else if (i > 0) {
                secondValue = ceil(i * 0.75);
            } else {
                secondValue = floor(i * 0.75);
            }
            fprintf(file, "%d,%.0f\n", i, secondValue);
        }
        fclose(file);
    }

    //------------ -25% sensitivity
    exists = 0;
    filepath = "/data/rgp2xbox/DecreaseAnalogSensitivityBy50Percent.csv";

    file = fopen(filepath, "r");
    if (file) {
        exists = 1;
        fclose(file);
    }
    if (!exists) {
        file = fopen(filepath, "w");
        if (file == NULL) {
            perror("Error opening file");
            return;
        }
        chmod(filepath, 0666);

        for (int i = -1800; i <= 1800; i++) {
            double secondValue;
            if (i == 0) {
                secondValue = 0;
            } else if (i > 0) {
                secondValue = ceil(i * 0.50);
            } else {
                secondValue = floor(i * 0.50);
            }
            fprintf(file, "%d,%.0f\n", i, secondValue);
        }
        fclose(file);
    }

    //------------ custom% sensitivity
    exists = 0;
    filepath = "/data/rgp2xbox/DecreaseAnalogSensitivityCustom.csv";

    file = fopen(filepath, "r");
    if (file) {
        exists = 1;
        fclose(file);
    }
    if (!exists) {
        file = fopen(filepath, "w");
        if (file == NULL) {
            perror("Error opening file");
            return;
        }
        chmod(filepath, 0666);

        for (int i = -1800; i <= 1800; i++) {
            fprintf(file, "%d,%d\n", i, i);
        }
        fclose(file);
    }
}

void clearLookupTable() {
    if (lookupTable != NULL) {
        free(lookupTable);
        lookupTable = NULL;
        lookupTableSize = 0;
    }
}

void readCSVAndBuildLookup(const char *filename) {
    clearLookupTable(); // Clear any existing data

    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        return;
    }

    lookupTable = (LookupEntry *)malloc(3600 * sizeof(LookupEntry));
    if (!lookupTable) {
        perror("Memory allocation failed");
        fclose(file);
        return;
    }

    char line[100];
    int i = 0;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%d,%d", &lookupTable[i].firstColumn, &lookupTable[i].secondColumn) == 2) {
            if (lookupTable[i].firstColumn != 0) {
                i++;
            }
        } else {
            clearLookupTable();
            fclose(file);
            return;
        }
    }
    lookupTableSize = i;
    fclose(file);
}

int lookupValue(int value) {
    if (lookupTable == NULL || lookupTableSize == 0) {
        return 0;
    }
    for (int i = 0; i < lookupTableSize; i++) {
        if (lookupTable[i].firstColumn == value) {
            return lookupTable[i].secondColumn;
        }
    }
    return 0;
}

void setAnalogSensitvityTable(int mode) {
    switch (mode) {
    case 1:
        readCSVAndBuildLookup("/data/rgp2xbox/DecreaseAnalogSensitivityBy15Percent.csv");
        break;
    case 2:
        readCSVAndBuildLookup("/data/rgp2xbox/DecreaseAnalogSensitivityBy25Percent.csv");
        break;
    case 3:
        readCSVAndBuildLookup("/data/rgp2xbox/DecreaseAnalogSensitivityBy50Percent.csv");
        break;
    case 4:
        readCSVAndBuildLookup("/data/rgp2xbox/DecreaseAnalogSensitivityCustom.csv");
        break;
    default:
        *analog_sensitivity = 0;
        *analog_axis_isupdated = 1;
        break;
    }
}
