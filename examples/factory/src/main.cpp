#include <vector>
#include <memory>
#include <cassert>
#include <memory>
#include <iostream>
#include <aff3ct.hpp>

constexpr float ebn0_min =  0.0f;
constexpr float ebn0_max =  2.1f;
constexpr float ebn0_step = 0.1f;

int main(int argc, char** argv)
{
	using namespace aff3ct;
	// declare the parameters objects
	factory::Source          ::parameters p_src;
	factory::Codec_turbo     ::parameters p_cdc;
	factory::CRC             ::parameters p_crc;
	factory::Modem           ::parameters p_mdm;
	factory::Channel         ::parameters p_chn;
	factory::Monitor_BFER    ::parameters p_mnt;
	factory::Terminal        ::parameters p_ter;
	p_cdc.enable_puncturer();

	std::vector<factory::Factory::parameters*> params = {&p_src, &p_cdc, &p_mdm, &p_chn, &p_mnt, &p_ter, &p_crc};

	factory::Command_parser cp(argc, argv, params, true);

	// parse the command for the given parameters and fill them
	if (cp.parsing_failed())
	{
		cp.print_help    ();
		cp.print_warnings();
		cp.print_errors  ();

		return EXIT_FAILURE;
	}

	// display the headers (= print the AFF3CT parameters on the screen)
	std::cout << "#-------------------------------------------------------" << std::endl;
	std::cout << "# This is a basic program using the AFF3CT library."      << std::endl;
	std::cout << "# Feel free to improve it as you want to fit your needs." << std::endl;
	std::cout << "#-------------------------------------------------------" << std::endl;
	std::cout << "#"                                                        << std::endl;
	factory::Header::print_parameters(params);
	std::cout << "#" << std::endl;

	cp.print_warnings();

	// create the AFF3CT modules
	std::unique_ptr<module::Source<>>           source (p_src.build());
	std::unique_ptr<module::Modem<>>            modem  (p_mdm.build());
	std::unique_ptr<module::Channel<>>          channel(p_chn.build());
	std::unique_ptr<module::Monitor_BFER<>>     monitor(p_mnt.build());
	std::unique_ptr<module::Codec_turbo<>>      codec  (p_cdc.build());
	std::unique_ptr<module::CRC<>>              crc    (p_crc.build());
	auto& encoder = codec->get_encoder();
	auto& pct     = codec->get_puncturer();
	auto& decoder = codec->get_decoder_siho();

	// create reporters to display results in terminal
	tools::Sigma<float>                  noise;
	std::vector<std::unique_ptr<tools::Reporter>> reporters;

	reporters.push_back(std::unique_ptr<tools::Reporter_noise<float>        >(new tools::Reporter_noise<float>        ( noise  ))); // reporter of the noise value
	reporters.push_back(std::unique_ptr<tools::Reporter_BFER <int>          >(new tools::Reporter_BFER <int>          (*monitor))); // reporter of the bit/frame error rate
	reporters.push_back(std::unique_ptr<tools::Reporter_throughput<uint64_t>>(new tools::Reporter_throughput<uint64_t>(*monitor))); // reporter of the throughput of the simulation

	// create a Terminal and display the legend
	std::unique_ptr<tools::Terminal> terminal(p_ter.build(reporters));
	terminal->legend();

	// configuration of the module tasks
	std::vector<const module::Module*> modules{source.get(), encoder.get(), modem.get(), channel.get(), decoder.get(), monitor.get(), crc.get(), pct.get()};
	for (auto& m : modules)
		for (auto& t : m->tasks)
		{
			t->set_autoalloc  (true ); // enable the automatic allocation of the data in the tasks
			t->set_autoexec   (false); // disable the auto execution mode of the tasks
			t->set_debug      (false); // disable the debug mode
			t->set_debug_limit(16   ); // display only the 16 first bits if the debug mode is enabled
			t->set_stats      (false); // enable the statistics

			// enable the fast mode (= disable the useless verifs in the tasks) if there is no debug and stats modes
			t->set_fast(!t->is_debug() && !t->is_stats());
		}

	// sockets binding (connect the sockets of the tasks = fill the input sockets with the output sockets)
	using namespace module;
	(*crc    )[crc::sck::build       ::U_K1].bind((*source )[src::sck::generate   ::U_K ]);
	(*encoder)[enc::sck::encode      ::U_K ].bind((*crc    )[crc::sck::build      ::U_K2]);
	(*pct    )[pct::sck::puncture    ::X_N1].bind((*encoder)[enc::sck::encode     ::X_N ]);
	(*modem  )[mdm::sck::modulate    ::X_N1].bind((*pct    )[pct::sck::puncture   ::X_N2 ]);
	(*channel)[chn::sck::add_noise   ::X_N ].bind((*modem  )[mdm::sck::modulate   ::X_N2]);
	(*modem  )[mdm::sck::demodulate  ::Y_N1].bind((*channel)[chn::sck::add_noise  ::Y_N ]);
	(*pct    )[pct::sck::depuncture  ::Y_N1].bind((*modem  )[mdm::sck::demodulate ::Y_N2]);
	(*decoder)[dec::sck::decode_siho ::Y_N ].bind((*pct    )[pct::sck::depuncture ::Y_N2]);
	(*crc    )[crc::sck::extract     ::V_K1].bind((*decoder)[dec::sck::decode_siho::V_K ]);
	(*monitor)[mnt::sck::check_errors::U   ].bind((*source )[src::sck::generate   ::U_K ]);
	(*monitor)[mnt::sck::check_errors::V   ].bind((*crc    )[crc::sck::extract    ::V_K2 ]);


	// reset the memory of the decoder after the end of each communication
	monitor->add_handler_check(std::bind(&module::Decoder::reset, decoder));

	// initialize the interleaver if this code use an interleaver
	try
	{
		auto& interleaver = codec->get_interleaver();
		interleaver->init();
	}
	catch (const std::exception&) { /* do nothing if there is no interleaver */ }

	// a loop over the various SNRs
	const float R = (float)p_cdc.enc->K / (float)p_cdc.enc->N_cw; // compute the code rate
	for (auto ebn0 = ebn0_min; ebn0 < ebn0_max; ebn0 += ebn0_step)
	{
		// compute the current sigma for the channel noise
		const auto esn0  = tools::ebn0_to_esn0 (ebn0, R);
		const auto sigma = tools::esn0_to_sigma(esn0   );

		noise.set_noise(sigma, ebn0, esn0);

		// update the sigma of the modem and the channel
		codec  ->set_noise(noise);
		modem  ->set_noise(noise);
		channel->set_noise(noise);

		// display the performance (BER and FER) in real time (in a separate thread)
		terminal->start_temp_report(p_ter.frequency);

		// run the simulation chain
		while (!monitor->fe_limit_achieved() && !tools::Terminal::is_interrupt())
		{
			(*source )[src::tsk::generate    ].exec();
			(*crc    )[crc::tsk::build       ].exec();
			(*encoder)[enc::tsk::encode      ].exec();
			(*pct    )[pct::tsk::puncture    ].exec();
			(*modem  )[mdm::tsk::modulate    ].exec();
			(*channel)[chn::tsk::add_noise   ].exec();
			(*modem  )[mdm::tsk::demodulate  ].exec();
			(*pct    )[pct::tsk::depuncture  ].exec();
			(*decoder)[dec::tsk::decode_siho ].exec();
			(*crc    )[crc::tsk::extract     ].exec();
			(*monitor)[mnt::tsk::check_errors].exec();
		}

		// display the performance (BER and FER) in the terminal
		terminal->final_report();

		if (tools::Terminal::is_over())
			break;

		// reset the monitor and the terminal for the next SNR
		monitor->reset();
		tools::Terminal::reset();
	}
	std::cout << "#" << std::endl;

	// display the statistics of the tasks (if enabled)
	auto ordered = true;
	tools::Stats::show(modules, ordered);

	// delete the aff3ct objects
	std::cout << "# End of the simulation" << std::endl;

	return EXIT_SUCCESS;
}
