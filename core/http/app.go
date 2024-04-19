package http

import (
	"encoding/json"
	"errors"
	"os"
	"strings"

	"github.com/go-skynet/LocalAI/pkg/utils"
	// swagger handler
	"github.com/go-skynet/LocalAI/core/http/endpoints/localai"
	"github.com/go-skynet/LocalAI/core/http/endpoints/openai"
	"github.com/go-skynet/LocalAI/core/http/routes"

	"github.com/go-skynet/LocalAI/core/config"
	"github.com/go-skynet/LocalAI/core/schema"
	"github.com/go-skynet/LocalAI/core/services"
	"github.com/go-skynet/LocalAI/pkg/model"

	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/fiber/v2/middleware/cors"
	"github.com/gofiber/fiber/v2/middleware/logger"
	"github.com/gofiber/fiber/v2/middleware/recover"
)

func readAuthHeader(c *fiber.Ctx) string {
	authHeader := c.Get("Authorization")

	// elevenlabs
	xApiKey := c.Get("xi-api-key")
	if xApiKey != "" {
		authHeader = "Bearer " + xApiKey
	}

	// anthropic
	xApiKey = c.Get("x-api-key")
	if xApiKey != "" {
		authHeader = "Bearer " + xApiKey
	}

	return authHeader
}

// @title LocalAI API
// @version 2.0.0
// @description The LocalAI Rest API.
// @termsOfService
// @contact.name LocalAI
// @contact.url https://localai.io
// @license.name MIT
// @license.url https://raw.githubusercontent.com/mudler/LocalAI/master/LICENSE
// @BasePath /
// @securityDefinitions.apikey BearerAuth
// @in header
// @name Authorization

func App(cl *config.BackendConfigLoader, ml *model.ModelLoader, appConfig *config.ApplicationConfig) (*fiber.App, error) {
	// Return errors as JSON responses
	app := fiber.New(fiber.Config{
		Views:                 renderEngine(),
		BodyLimit:             appConfig.UploadLimitMB * 1024 * 1024, // this is the default limit of 4MB
		DisableStartupMessage: appConfig.DisableMessage,
		// Override default error handler
		ErrorHandler: func(ctx *fiber.Ctx, err error) error {
			// Status code defaults to 500
			code := fiber.StatusInternalServerError

			// Retrieve the custom status code if it's a *fiber.Error
			var e *fiber.Error
			if errors.As(err, &e) {
				code = e.Code
			}

			// Send custom error page
			return ctx.Status(code).JSON(
				schema.ErrorResponse{
					Error: &schema.APIError{Message: err.Error(), Code: code},
				},
			)
		},
	})

	if appConfig.Debug {
		app.Use(logger.New(logger.Config{
			Format: "[${ip}]:${port} ${status} - ${method} ${path}\n",
		}))
	}

	// Default middleware config

	if !appConfig.Debug {
		app.Use(recover.New())
	}

	metricsService, err := services.NewLocalAIMetricsService()
	if err != nil {
		return nil, err
	}

	if metricsService != nil {
		app.Use(localai.LocalAIMetricsAPIMiddleware(metricsService))
		app.Hooks().OnShutdown(func() error {
			return metricsService.Shutdown()
		})
	}

	// Auth middleware checking if API key is valid. If no API key is set, no auth is required.
	auth := func(c *fiber.Ctx) error {
		if len(appConfig.ApiKeys) == 0 {
			return c.Next()
		}

		// Check for api_keys.json file
		fileContent, err := os.ReadFile("api_keys.json")
		if err == nil {
			// Parse JSON content from the file
			var fileKeys []string
			err := json.Unmarshal(fileContent, &fileKeys)
			if err != nil {
				return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{"message": "Error parsing api_keys.json"})
			}

			// Add file keys to options.ApiKeys
			appConfig.ApiKeys = append(appConfig.ApiKeys, fileKeys...)
		}

		if len(appConfig.ApiKeys) == 0 {
			return c.Next()
		}

		authHeader := readAuthHeader(c)
		if authHeader == "" {
			return c.Status(fiber.StatusUnauthorized).JSON(fiber.Map{"message": "Authorization header missing"})
		}

		// If it's a bearer token
		authHeaderParts := strings.Split(authHeader, " ")
		if len(authHeaderParts) != 2 || authHeaderParts[0] != "Bearer" {
			return c.Status(fiber.StatusUnauthorized).JSON(fiber.Map{"message": "Invalid Authorization header format"})
		}

		apiKey := authHeaderParts[1]
		for _, key := range appConfig.ApiKeys {
			if apiKey == key {
				return c.Next()
			}
		}

		return c.Status(fiber.StatusUnauthorized).JSON(fiber.Map{"message": "Invalid API key"})
	}

	if appConfig.CORS {
		var c func(ctx *fiber.Ctx) error
		if appConfig.CORSAllowOrigins == "" {
			c = cors.New()
		} else {
			c = cors.New(cors.Config{AllowOrigins: appConfig.CORSAllowOrigins})
		}

		app.Use(c)
	}

	// LocalAI API endpoints
	galleryService := services.NewGalleryService(appConfig.ModelPath)
	galleryService.Start(appConfig.Context, cl)

	// Make sure directories exists
	os.MkdirAll(appConfig.ImageDir, 0755)
	os.MkdirAll(appConfig.AudioDir, 0755)
	os.MkdirAll(appConfig.UploadDir, 0755)
	os.MkdirAll(appConfig.ConfigsDir, 0755)
	os.MkdirAll(appConfig.ModelPath, 0755)

	// Load config jsons
	utils.LoadConfig(appConfig.UploadDir, openai.UploadedFilesFile, &openai.UploadedFiles)
	utils.LoadConfig(appConfig.ConfigsDir, openai.AssistantsConfigFile, &openai.Assistants)
	utils.LoadConfig(appConfig.ConfigsDir, openai.AssistantsFileConfigFile, &openai.AssistantFiles)

	routes.RegisterWelcomeRoute(
		app,
		cl,
		ml,
		appConfig,
		auth,
	)
	routes.RegisterAPIRoutes(appConfig, cl, ml, app, auth, galleryService)
	routes.RegisterChatRoute(app, cl, ml, appConfig, auth)

	if appConfig.ImageDir != "" {
		app.Static("/generated-images", appConfig.ImageDir)
	}

	if appConfig.AudioDir != "" {
		app.Static("/generated-audio", appConfig.AudioDir)
	}

	// Define a custom 404 handler
	// Note: keep this at the bottom!
	app.Use(notFoundHandler)

	return app, nil
}
