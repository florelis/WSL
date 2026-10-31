// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"

int wmain(int argc, wchar_t** argv)
{
    using namespace wsl::windows::compose;
    try
    {
        if (argc != 2)
        {
            std::cerr << "Usage: compose-parser-poc absolute-library-path < request.json\n";
            return 2;
        }
        std::string input;
        char buffer[8192];
        while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount())
        {
            if (input.size() + static_cast<size_t>(std::cin.gcount()) > c_maxRequestBytes)
            {
                std::cout << EncodeResult(Failure("limit_exceeded", "Request exceeds 4 MiB")).dump() << '\n';
                return 2;
            }
            input.append(buffer, static_cast<size_t>(std::cin.gcount()));
        }
        if (!std::cin.eof())
        {
            throw std::runtime_error("Failed to read request");
        }
        Request request;
        try
        {
            request = DecodeRequest(ParseJson(input));
        }
        catch (const std::exception& error)
        {
            std::cout << EncodeResult(Failure("invalid_request", error.what())).dump() << '\n';
            return 2;
        }
        ParserOptions options;
        if (argc == 2)
        {
            options.LibraryPath = argv[1];
        }
        auto result = Normalize(request, options);
        std::cout << EncodeResult(result).dump() << '\n';
        return result.AcceptedProject ? 0 : 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
