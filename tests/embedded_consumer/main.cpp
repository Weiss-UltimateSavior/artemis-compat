extern "C" bool artemis_embedded_probe();
int main() { return artemis_embedded_probe() ? 0 : 1; }
