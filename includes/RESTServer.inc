<?php

class RESTServer {
  private $endpoint;

  /**
   * Handles the call to the REST server
   *
   * @param string $canonical_path
   * @param string $endpoint_path
   * @return void
   * @author Hugo Wetterberg
   */
  public function handle($canonical_path, $endpoint_path) {
    $this->endpoint = $endpoint_path;
    services_resource_uri_formatter(array($this, 'uri_formatter'));

    // Determine the request method
    $method = $_SERVER['REQUEST_METHOD'];
    if ($method=='POST' && $_GET['_method']) {
      $method = $_GET['_method'];
    }

    $path = split('/', $canonical_path);
    $leaf = array_pop($path);
    $resource_name = array_shift($path);

    // Extract response format info from the path
    $matches=array();
    if ($leaf && preg_match('/^(.+)\.([^\.]+)$/', $leaf, $matches)) {
      $leaf = $matches[1];
      $response_format = $matches[2];
    }
    // Response will vary with accept headers
    // if no format was supplied as path suffix
    if (empty($response_format)) {
      header('Vary: Accept');
    }

    // Return the leaf to the path array if it's not the resource name
    if ($leaf) {
      if (!$resource_name) {
        $resource_name = $leaf;
      }
      else {
        array_push($path, $leaf);
      }
    }

    $resources = services_get_all_resources();
    $controller = FALSE;
    if (!empty($resource_name) && isset($resources[$resource_name])) {
      $resource = $resources[$resource_name];

      // Get the operation and fill with default values
      $controller = $this->resolveOperation($resource, $method, $path);
      $this->setDefaultOperationAttributes($controller);

      // Load required files
      if (isset($controller['#file'])) {
        $this->loadInclude($controller['#file']);
      }
      else if (isset($resource['#file'])) {
        $this->loadInclude($resource['#file']);
      }

      drupal_alter('rest_server_controller', $controller, $resource);
    }

    if (!$controller) {
      header("HTTP/1.0 404 Not found");
      exit;
    }

    // Parse the request data
    try {
      $arguments = $this->getControllerArguments($controller, $path, $method);
    }
    catch (Exception $e) {
      header('HTTP/1.0 406 Not Acceptable: ' . $e->getMessage());
      exit;
    }

    $formats = $this->responseFormatters();

    // Negotiate response format based on accept-headers if we
    // don't have a response format
    if (empty($response_format)) {
      module_load_include('php', 'rest_server', 'lib/mimeparse');

      $mime_candidates = array();
      $mime_map = array();
      // Add all formatters that accepts raw data, or supports the format model
      foreach ($formats as $format => $formatter) {
        if (!isset($formatter['model']) || $this->supportedControllerModel($controller, $formatter)) {
          foreach ($formatter['mime types'] as $m) {
            $mime_candidates[] = $m;
            $mime_map[$m] = $format;
          }
        }
      }

      // Get the best matching format, default to json
      $mime = new Mimeparse();
      $mime_type = $mime->best_match($mime_candidates, $_SERVER['HTTP_ACCEPT']);
      if ($mime_type) {
        $response_format = $mime_map[$mime_type];
      }
      else {
        $response_format = 'json';
      }
    }

    // Check if we support the response format and determine the mime type
    if (empty($mime_type) && !empty($response_format) && isset($formats[$response_format])) {
      $formatter = $formats[$response_format];
      if (!isset($formatter['model']) || $this->supportedControllerModel($controller, $formatter)) {
        $mime_type = $formatter['mime types'][0];
      }
    }

    if (empty($response_format) || empty($mime_type)) {
      header("HTTP/1.0 406 Not Acceptable: unknown or unsupported response format.");
      exit;
    }

    // Set the content type
    header('Content-type: ' . $mime_type);

    $result = call_user_func_array($controller['#callback'], $arguments);
    $formatter = $formats[$response_format];
    return $this->renderFormatterView($formatter, $result);
  }

  /**
   * Formats a resource uri
   *
   * @param array $path
   *  An array of strings containing the component parts of the path to the resource.
   * @return string
   *  Returns the formatted resource uri
   */
  public function uri_formatter($path) {
    return url($this->endpoint . '/' . join($path, '/'));
  }

  private function getControllerArguments($controller, $path, $method) {
    // Get argument sources
    $parameters = $_GET;
    unset($parameters['_method']);
    $data = $this->parseRequest($method);

    $sources = array(
      'path' => $path,
      'param' => $parameters,
      'data' => $data,
    );

    // Map source data to arguments.
    if (isset($controller['#args'])) {
      foreach ($controller['#args'] as $name => $info) {
        // Fill in argument from source
        if (isset($info['#source'])) {
          if (is_array($info['#source'])) {
            list($source) = array_keys($info['#source']);
            $key = $info['#source'][$source];
            if (isset($sources[$source][$key])) {
              $arguments[$name] = $sources[$source][$key];
            }
          }
          else {
            if (isset($sources[$info['#source']])) {
              $arguments[$name] = $sources[$info['#source']];
            }
          }
        }

        // When argument isn't set, insert default value if provided or
        // throw a exception if the argument isn't optional.
        if (!isset($arguments[$name])) {
          if (isset($info['#default value'])) {
            $arguments[$name] = $info['#default value'];
          }
          else if (!isset($info['#optional']) || !$info['#optional']) {
            throw new Exception(t('Missing required argument !arg', array('!arg'=>$name)));
          }
        }
      }
    }
    return $arguments;
  }

  private function parseRequest($method) {
    switch ($method) {
      case 'POST':
        if ($_SERVER['CONTENT_TYPE'] == 'application/x-www-form-urlencoded') {
          return $_POST;
        }
      case 'PUT':
        // Read the raw input stream
        $handle = fopen("php://input", "r");
        $content = '';
        while (!feof($handle)) {
          $content .= fread($handle, 8192);
        }

        // Get the parser for the request content type, default to form-urlencoded
        $mime = isset($_SERVER['CONTENT_TYPE'])?$_SERVER['CONTENT_TYPE']:'application/x-www-form-urlencoded';
        $parser = $this->requestParsers($mime);

        if (!$parser) {
          throw new Exception(t('Unsupported request content type !mime', array('!mime'=>$mime)));
        }

        $data = call_user_func($parser, $content);
        return $data;

      default:
        return array();
    }
  }

  private function renderFormatterView($formatter, $result) {
    // Wrap the results in a model class if required by the formatter
    if (isset($formatter['model'])) {
      $cm = $controller['#models'][$formatter['model']];
      $model_arguments = isset($cm['arguments'])?$cm['arguments']:array();

      $model_class = new ReflectionClass($cm['class']);
      $result = $model_class->newInstanceArgs(array($result, $model_arguments));
    }

    $view_class = new ReflectionClass($formatter['view']);
    $view_arguments = isset($formatter['view arguments'])?$formatter['view arguments']:array();
    $view = $view_class->newInstanceArgs(array($result, $view_arguments));
    return $view->render();
  }

  private function requestParsers($mime=NULL) {
    static $parsers;

    if (!$parsers) {
      $parsers = array(
        'application/x-www-form-urlencoded' => 'RESTServer::parseURLEncoded',
        'application/x-yaml' => 'RESTServer::parseYAML',
        'application/json' => 'RESTServer::parseJSON',
        'application/vnd.php.serialized' => 'RESTServer::parsePHP',
      );
      drupal_alter('rest_server_request_parsers', $parsers);
    }

    if ($mime) {
      return isset($parsers[$mime]) ? $parsers[$mime] : FALSE;
    }
    return $parsers;
  }

  public static function parseURLEncoded($data) {
    $data = array();
    parse_str($content, $data);
    return $data;
  }

  public static function parsePHP($data) {
    return unserialize($data);
  }

  public static function parseJSON($data) {
    return json_decode($data);
  }

  public static function parseYAML($data) {
    module_load_include('php', 'rest_server', 'lib/spyc');
    return Spyc::YAMLLoad($data);
  }

  private function responseFormatters($format=NULL) {
    static $formatters;

    if (!$formatters) {
      $formatters = array(
        'xml' => array(
          'mime types' => array('application/xml', 'text/xml'),
          'view' => 'RESTServerViewBuiltIn',
          'view arguments' => array('format'=>'xml'),
        ),
        'json' => array(
          'mime types' => array('application/json'),
          'view' => 'RESTServerViewBuiltIn',
          'view arguments' => array('format'=>'json'),
        ),
        'php' => array(
          'mime types' => array('application/vnd.php.serialized'),
          'view' => 'RESTServerViewBuiltIn',
          'view arguments' => array('format'=>'php'),
        ),
        'yaml' => array(
          'mime types' => array('text/plain', 'application/x-yaml', 'text/yaml'),
          'view' => 'RESTServerViewBuiltIn',
          'view arguments' => array('format'=>'yaml'),
        ),
        'bencode' => array(
          'mime types' => array('application/x-bencode'),
          'view' => 'RESTServerViewBuiltIn',
          'view arguments' => array('format'=>'bencode'),
        ),
      );
      drupal_alter('rest_server_response_formatters', $formatters);
    }

    if ($format) {
      return isset($formatters[$format]) ? $formatters[$format] : FALSE;
    }
    return $formatters;
  }

  private function setDefaultOperationAttributes(&$op) {
    if (!isset($op['#authenticate'])) {
      $op['#authenticate'] = TRUE;
    }

    if (!isset($op['#key'])) {
      $op['#key'] = TRUE;
    }
  }

  private function loadInclude($file) {
    module_load_include($file['file'], $file['module'], isset($file['name'])?$file['name']:NULL);
  }

  private function supportedControllerModel($controller, $format) {
    if (
      // The format uses models
      isset($format['model']) &&
      // The controller provides models
      isset($controller['#models']) &&
      // The controller supports the model required by the format
      isset($controller['#models'][$format['model']])) {
        return $controller['#models'][$format['model']];
    }
  }

  private function resolveOperation($resource, $method, $path) {
    $action_mapping = array(
      'GET' => '#retrieve',
      'POST' => '#create',
      'PUT' => '#update',
      'DELETE' => '#delete',
    );

    $handler = $resource;
    // Use the index handler for all empty path request, except on POST
    if (empty($path) && $method!='POST') {
      $handler = isset($resource['#index']) ? $resource['#index'] : NULL;
    }

    if ($handler && isset($handler[$action_mapping[$method]])) {
      return $handler[$action_mapping[$method]];
    }
  }
}