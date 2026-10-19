#ifndef SPEECH_COMPCHECK_H
#define SPEECH_COMPCHECK_H

#include <string>
#include <vector>

/**
 * Turns the model's answer into a corrected text (SPEC §7.5). A pure class.
 *
 * The model gives word pairs, never positions nor a rewritten text: this
 * class finds each word in the text sent, and keeps only what is a
 * correction: at most 2 letters changed, or a third of the word.
 */
class CompCheck
{
public:
	struct Change
	{
		size_t		start;	// UTF-16 code units, in the text sent
		size_t		end;
		std::string	from;
		std::string	to;
	};

	struct Result
	{
		bool			valid = false;	// false: the answer was not readable
		std::string		text;		// corrected text
		std::vector<Change>	changes;
	};

	// `answer` is the content of the model's message.
	static Result Apply(const std::string& text, const std::string& answer);
};

#endif
