#include <iostream>
#include <memory>
#include <vector>
#include <string>

#include <aff3ct.hpp>
using namespace aff3ct;

int main(int argc, char** argv)
{
	int K = 4;
	int N = 8;
	const float R        = (float)K / (float)N;
	tools::Frozenbits_generator_GA      fb_gen (K, N);

	const float ebn0  = 2.f;
	const auto  esn0  = tools::ebn0_to_esn0 (ebn0, R);
	const auto  sigma = tools::esn0_to_sigma(esn0   );
	tools::Sigma<> noise(sigma, ebn0, esn0);
	fb_gen.set_noise(noise);

	std::vector<bool> frozen_bits(N);
	fb_gen.generate(frozen_bits);

	for (auto i = 0; i < frozen_bits.size(); i++)
		std::cout << frozen_bits[i] << " ";
	std::cout << std::endl;

	return 0;
}
